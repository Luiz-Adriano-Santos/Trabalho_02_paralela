// servidor.c (Versão Final para Mac/Linux com Cache FIFO e Erros Numéricos)
// Compilar com: gcc servidor.c -o servidor -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define BASE_PORT 7000
#define MAX_BUFFER_SIZE 8192
#define MAX_CONEXOES 20

// Códigos de status e erro
#define SUCESSO 0
#define ERRO_GENERICO -1
#define ERRO_MEMORIA_INEXISTENTE -2
#define ERRO_COMANDO_DESCONHECIDO -3
#define ERRO_FALHA_OBTER_BLOCO -4

// Estruturas de Dados
typedef struct {
    int id;
    char* dados;
} BlocoMemoria;

typedef struct {
    int id;
    char* dados;
    int valido;
} BlocoCache;

// --- Variáveis Globais ---
int N_PROCESSOS = 0;
int K_BLOCOS = 0;
int T_BLOCO = 0;
int my_rank = 0;

BlocoMemoria* blocos_locais = NULL;
int num_blocos_locais = 0;

// Lógica de Cache FIFO
BlocoCache* cache = NULL;
int tamanho_cache = 0;
int proximo_slot_cache = 0;
pthread_mutex_t cache_mutex;

// --- Implementação das Funções ---

void die(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int recv_all(int sock, char* buffer, int len) {
    int total_recebido = 0;
    int bytes_restantes = len;
    while(total_recebido < len) {
        int bytes_recebidos = recv(sock, buffer + total_recebido, bytes_restantes, 0);
        if (bytes_recebidos <= 0) {
            return -1; // Erro ou conexão fechada
        }
        total_recebido += bytes_recebidos;
        bytes_restantes -= bytes_recebidos;
    }
    return total_recebido;
}

int calcular_dono(int id_bloco) {
    if (id_bloco < 0 || id_bloco >= K_BLOCOS) return -1;
    int blocos_por_processo = K_BLOCOS / N_PROCESSOS;
    if (blocos_por_processo == 0 && K_BLOCOS > 0) return id_bloco % N_PROCESSOS;
    if (blocos_por_processo == 0) return -1;
    int dono = id_bloco / blocos_por_processo;
    return (dono >= N_PROCESSOS) ? N_PROCESSOS - 1 : dono;
}

void mapear_posicao_global(int posicao, int* id_bloco, int* offset_bloco) {
    *id_bloco = posicao / T_BLOCO;
    *offset_bloco = posicao % T_BLOCO;
}

void enviar_msg_assincrona(int rank_destino, const char* msg) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    struct sockaddr_in server;
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_family = AF_INET;
    server.sin_port = htons(BASE_PORT + rank_destino);
    if (connect(s, (struct sockaddr *)&server, sizeof(server)) >= 0) {
        send(s, msg, strlen(msg), 0);
    }
    close(s);
}

void adicionar_bloco_na_cache(int id_bloco, char* dados_bloco) {
    pthread_mutex_lock(&cache_mutex);
    int slot_vitima = proximo_slot_cache;
    printf("[P%d] Cache: Adicionando bloco %d no slot %d (substituindo bloco %d).\n", 
           my_rank, id_bloco, slot_vitima, cache[slot_vitima].id);
    cache[slot_vitima].id = id_bloco;
    memcpy(cache[slot_vitima].dados, dados_bloco, T_BLOCO);
    cache[slot_vitima].valido = 1;
    proximo_slot_cache = (proximo_slot_cache + 1) % tamanho_cache;
    pthread_mutex_unlock(&cache_mutex);
}

int obter_bloco_remoto(int id_bloco, char* buffer_retorno) {
    int dono = calcular_dono(id_bloco);
    if (dono < 0) return -1;
    char msg[100];
    sprintf(msg, "OBTER_BLOCO_INTERNO %d", id_bloco);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in server;
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_family = AF_INET;
    server.sin_port = htons(BASE_PORT + dono);
    if (connect(s, (struct sockaddr *)&server, sizeof(server)) < 0) {
        close(s);
        return -1;
    }
    send(s, msg, strlen(msg), 0);
    int recv_size = recv_all(s, buffer_retorno, T_BLOCO);
    close(s);
    if (recv_size == T_BLOCO) {
        adicionar_bloco_na_cache(id_bloco, buffer_retorno);
        return 0;
    }
    return -1;
}

void* handle_connection(void* socket_desc) {
    int sock = *(int*)socket_desc;
    free(socket_desc);
    char buffer_req[MAX_BUFFER_SIZE];
    int read_size;

    memset(buffer_req, 0, MAX_BUFFER_SIZE);
    if ((read_size = recv(sock, buffer_req, MAX_BUFFER_SIZE, 0)) > 0) {
        buffer_req[read_size] = '\0';
        printf("[P%d] Recebeu: '%.50s...'\n", my_rank, buffer_req);

        char command[30];
        sscanf(buffer_req, "%s", command);

        if (strcmp(command, "OBTER_DADOS") == 0) {
            int pos, tam;
            sscanf(buffer_req, "%*s %d %d", &pos, &tam);
            if (pos < 0 || (pos + tam) > (K_BLOCOS * T_BLOCO)) {
                int codigo_erro = ERRO_MEMORIA_INEXISTENTE;
                send(sock, &codigo_erro, sizeof(int), 0);
            } else {
                char* resultado = malloc(tam);
                int bytes_coletados = 0;
                int falha_geral = 0;
                for (int p_atual = pos; p_atual < pos + tam; ) {
                    int id_bloco, offset;
                    mapear_posicao_global(p_atual, &id_bloco, &offset);
                    int dono = calcular_dono(id_bloco);
                    char* fonte_dados = malloc(T_BLOCO);
                    int sucesso_leitura = 0;

                    if (dono == my_rank) {
                        for(int i = 0; i < num_blocos_locais; i++) {
                            if (blocos_locais[i].id == id_bloco) {
                                memcpy(fonte_dados, blocos_locais[i].dados, T_BLOCO);
                                sucesso_leitura = 1;
                                printf("[P%d] Acesso LOCAL ao bloco %d.\n", my_rank, id_bloco);
                                break;
                            }
                        }
                    } else {
                        pthread_mutex_lock(&cache_mutex);
                        for(int i = 0; i < tamanho_cache; i++){
                            if(cache[i].id == id_bloco && cache[i].valido){
                                memcpy(fonte_dados, cache[i].dados, T_BLOCO);
                                sucesso_leitura = 1;
                                printf("[P%d] Acesso via CACHE ao bloco %d (slot %d).\n", my_rank, id_bloco, i);
                                break;
                            }
                        }
                        pthread_mutex_unlock(&cache_mutex);

                        if (!sucesso_leitura) {
                            printf("[P%d] CACHE MISS. Buscando bloco %d do P%d.\n", my_rank, id_bloco, dono);
                            if (obter_bloco_remoto(id_bloco, fonte_dados) == 0) {
                                sucesso_leitura = 1;
                            }
                        }
                    }

                    if (sucesso_leitura) {
                        int bytes_a_ler = T_BLOCO - offset;
                        if (bytes_a_ler > (tam - bytes_coletados)) {
                            bytes_a_ler = tam - bytes_coletados;
                        }
                        memcpy(resultado + bytes_coletados, fonte_dados + offset, bytes_a_ler);
                        bytes_coletados += bytes_a_ler;
                        p_atual += bytes_a_ler;
                    } else { 
                        falha_geral = 1; 
                        free(fonte_dados); 
                        break; 
                    }
                    free(fonte_dados);
                }
                
                if(falha_geral) {
                    int codigo_erro = ERRO_FALHA_OBTER_BLOCO;
                    send(sock, &codigo_erro, sizeof(int), 0);
                } else {
                    int status_sucesso = SUCESSO;
                    send(sock, &status_sucesso, sizeof(int), 0);
                    send(sock, resultado, tam, 0);
                }
                free(resultado);
            }
        } else if (strcmp(command, "OBTER_BLOCO_INTERNO") == 0) {
            int id_bloco;
            sscanf(buffer_req, "%*s %d", &id_bloco);
            for(int i=0; i < num_blocos_locais; i++){
                if(blocos_locais[i].id == id_bloco) {
                    send(sock, blocos_locais[i].dados, T_BLOCO, 0);
                    break;
                }
            }
        } else if (strcmp(command, "SALVAR_DADOS") == 0) {
            int pos, tam;
            char* dados_a_salvar = buffer_req;
            sscanf(buffer_req, "%*s %d %d", &pos, &tam);
            dados_a_salvar = strchr(dados_a_salvar, ' ') + 1; // Pula comando
            dados_a_salvar = strchr(dados_a_salvar, ' ') + 1; // Pula pos
            dados_a_salvar = strchr(dados_a_salvar, ' ') + 1; // Pula tam
            
            if (pos < 0 || (pos + tam) > (K_BLOCOS * T_BLOCO)) {
                int codigo_erro = ERRO_MEMORIA_INEXISTENTE;
                send(sock, &codigo_erro, sizeof(int), 0);
            } else {
                 for (int i = 0; i < tam; ) {
                    int p_atual = pos + i;
                    int id_bloco, offset;
                    mapear_posicao_global(p_atual, &id_bloco, &offset);
                    int dono = calcular_dono(id_bloco);
                    int bytes_a_escrever = T_BLOCO - offset;
                    if (bytes_a_escrever > (tam - i)) {
                        bytes_a_escrever = tam - i;
                    }
                    char msg_remota[MAX_BUFFER_SIZE];
                    sprintf(msg_remota, "ATUALIZAR_BLOCO %d %d %d ", id_bloco, offset, bytes_a_escrever);
                    memcpy(msg_remota + strlen(msg_remota), dados_a_salvar + i, bytes_a_escrever);
                    enviar_msg_assincrona(dono, msg_remota);
                    i += bytes_a_escrever;
                }
                int status_sucesso = SUCESSO;
                send(sock, &status_sucesso, sizeof(int), 0);
            }
        } else if (strcmp(command, "ATUALIZAR_BLOCO") == 0) {
            int id_bloco, offset, tam;
            sscanf(buffer_req, "%*s %d %d %d", &id_bloco, &offset, &tam);
            char* dados_recebidos = strstr(buffer_req, " ") + 1;
            dados_recebidos = strstr(dados_recebidos, " ") + 1;
            dados_recebidos = strstr(dados_recebidos, " ") + 1;

            for(int i = 0; i < num_blocos_locais; i++){
                if(blocos_locais[i].id == id_bloco){
                    memcpy(blocos_locais[i].dados + offset, dados_recebidos, tam);
                    printf("[P%d] Bloco LOCAL %d atualizado.\n", my_rank, id_bloco);
                    char msg_invalidacao[50];
                    sprintf(msg_invalidacao, "INVALIDAR_BLOCO %d", id_bloco);
                    for(int p = 0; p < N_PROCESSOS; p++) {
                        if (p != my_rank) {
                            enviar_msg_assincrona(p, msg_invalidacao);
                        }
                    }
                    break;
                }
            }
        } else if (strcmp(command, "INVALIDAR_BLOCO") == 0) {
            int id_bloco;
            sscanf(buffer_req, "%*s %d", &id_bloco);
            pthread_mutex_lock(&cache_mutex);
            for(int i = 0; i < tamanho_cache; i++){
                if(cache[i].id == id_bloco && cache[i].valido){
                    cache[i].valido = 0;
                    printf("[P%d] Cache para o bloco %d (slot %d) INVALIDADA.\n", my_rank, id_bloco, i);
                    break;
                }
            }
            pthread_mutex_unlock(&cache_mutex);
        } else {
             int codigo_erro = ERRO_COMANDO_DESCONHECIDO;
             send(sock, &codigo_erro, sizeof(int), 0);
        }
    }
    close(sock);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <num_processos> <num_blocos> <tamanho_bloco>\n", argv[0]);
        exit(1);
    }
    N_PROCESSOS = atoi(argv[1]);
    K_BLOCOS = atoi(argv[2]);
    T_BLOCO = atoi(argv[3]);
    if(N_PROCESSOS <= 0 || K_BLOCOS <=0 || T_BLOCO <=0) {
        fprintf(stderr, "Argumentos devem ser numeros positivos.\n");
        exit(1);
    }

    pid_t pids[N_PROCESSOS];
    for (int i = 0; i < N_PROCESSOS - 1; i++) {
        pids[i] = fork();
        if (pids[i] < 0) die("fork falhou");
        if (pids[i] == 0) { my_rank = i + 1; break; }
    }

    printf("[P%d] Iniciado. PID: %d\n", my_rank, getpid());

    int blocos_por_processo = K_BLOCOS / N_PROCESSOS;
    int blocos_inicio = my_rank * blocos_por_processo;
    int blocos_fim = (my_rank == N_PROCESSOS - 1) ? (K_BLOCOS - 1) : (blocos_inicio + blocos_por_processo - 1);
    num_blocos_locais = blocos_fim - blocos_inicio + 1;
    
    if (num_blocos_locais > 0) {
        blocos_locais = malloc(sizeof(BlocoMemoria) * num_blocos_locais);
        for(int i = 0; i < num_blocos_locais; i++){
            blocos_locais[i].id = blocos_inicio + i;
            blocos_locais[i].dados = malloc(T_BLOCO);
            memset(blocos_locais[i].dados, '-', T_BLOCO);
        }
    }
    printf("[P%d] Responsavel pelos blocos de %d a %d.\n", my_rank, blocos_inicio, blocos_fim);
    
    tamanho_cache = (int)(K_BLOCOS * 0.20);
    if (tamanho_cache == 0 && K_BLOCOS > 0) tamanho_cache = 1;
    
    if(tamanho_cache > 0) {
        printf("[P%d] Tamanho da cache: %d blocos.\n", my_rank, tamanho_cache);
        cache = malloc(sizeof(BlocoCache) * tamanho_cache);
        for(int i = 0; i < tamanho_cache; i++){
            cache[i].id = -1;
            cache[i].dados = malloc(T_BLOCO);
            cache[i].valido = 0; 
        }
    }
    pthread_mutex_init(&cache_mutex, NULL);

    int listening_socket;
    struct sockaddr_in server;
    listening_socket = socket(AF_INET, SOCK_STREAM, 0);
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(BASE_PORT + my_rank);

    if (bind(listening_socket, (struct sockaddr*)&server, sizeof(server)) < 0) die("bind falhou");
    listen(listening_socket, MAX_CONEXOES);
    printf("[P%d] Escutando na porta %d...\n", my_rank, BASE_PORT + my_rank);

    while (1) {
        int client_sock = accept(listening_socket, NULL, NULL);
        if (client_sock < 0) continue;
        pthread_t sniffer_thread;
        int* new_sock = malloc(sizeof(int));
        *new_sock = client_sock;
        if (pthread_create(&sniffer_thread, NULL, handle_connection, (void*)new_sock) < 0) {
            perror("nao foi possivel criar a thread");
        }
    }
    
    return 0;
}