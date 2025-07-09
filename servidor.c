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

void die(const char* msg);
void adicionar_bloco_na_cache(int id_bloco, char* dados_bloco);


int recv_all(int sock, char* buffer, int len) {
    int total_recebido = 0;
    int bytes_restantes = len;
    while(total_recebido < len) {
        int bytes_recebidos = recv(sock, buffer + total_recebido, bytes_restantes, 0);
        if (bytes_recebidos <= 0) {
            return -1;
        }
        total_recebido += bytes_recebidos;
        bytes_restantes -= bytes_recebidos;
    }
    return total_recebido;
}

void die(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int calcular_dono(int id_bloco) {
    if (id_bloco < 0 || id_bloco >= K_BLOCOS) return -1;
    int blocos_por_processo = K_BLOCOS / N_PROCESSOS;
    if (blocos_por_processo == 0) return -1; // Evita divisão por zero se N > K
    int dono = id_bloco / blocos_por_processo;
    return (dono >= N_PROCESSOS) ? N_PROCESSOS - 1 : dono;
}

void mapear_posicao_global(int posicao, int* id_bloco, int* offset_bloco) {
    *id_bloco = posicao / T_BLOCO;
    *offset_bloco = posicao % T_BLOCO;
}

void enviar_msg_assincrona(int rank_destino, const char* msg) {
    int s;
    struct sockaddr_in server;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_family = AF_INET;
    server.sin_port = htons(BASE_PORT + rank_destino);
    if (connect(s, (struct sockaddr *)&server, sizeof(server)) >= 0) {
        send(s, msg, strlen(msg), 0);
    }
    close(s);
}

int obter_bloco_remoto(int id_bloco, char* buffer_retorno) {
    int dono = calcular_dono(id_bloco);
    char msg[100];
    sprintf(msg, "OBTER_BLOCO_INTERNO %d", id_bloco);
    
    int s;
    struct sockaddr_in server;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    
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
        // Adiciona na cache usando a política FIFO
        adicionar_bloco_na_cache(id_bloco, buffer_retorno);
        return 0;
    }
    return -1;
}

void adicionar_bloco_na_cache(int id_bloco, char* dados_bloco) {
    pthread_mutex_lock(&cache_mutex);

    // O próximo slot a ser substituído é o "mais antigo"
    int slot_vitima = proximo_slot_cache;
    
    printf("[P%d] Cache: Adicionando bloco %d no slot %d (substituindo bloco %d).\n", 
           my_rank, id_bloco, slot_vitima, cache[slot_vitima].id);

    // Copia os novos dados para o slot da vítima
    cache[slot_vitima].id = id_bloco;
    memcpy(cache[slot_vitima].dados, dados_bloco, T_BLOCO);
    cache[slot_vitima].valido = 1;

    // Avança o ponteiro do FIFO para o próximo slot
    proximo_slot_cache = (proximo_slot_cache + 1) % tamanho_cache;

    pthread_mutex_unlock(&cache_mutex);
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
            // ... (lógica de leitura, igual à anterior, mas a busca na cache mudou)
            int pos, tam;
            sscanf(buffer_req, "%*s %d %d", &pos, &tam);

            if (pos < 0 || (pos + tam) > (K_BLOCOS * T_BLOCO)) {
                send(sock, "ERRO 2 Memoria fora dos limites", 32, 0);
            } else {
                char* resultado = malloc(tam + 1);
                int bytes_coletados = 0;

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
                    } else { // Bloco remoto
                        pthread_mutex_lock(&cache_mutex);
                        for(int i = 0; i < tamanho_cache; i++){ // Busca na cache de tamanho limitado
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
                        int bytes_a_ler_neste_bloco = T_BLOCO - offset;
                        if (bytes_a_ler_neste_bloco > (tam - bytes_coletados)) {
                            bytes_a_ler_neste_bloco = tam - bytes_coletados;
                        }
                        memcpy(resultado + bytes_coletados, fonte_dados + offset, bytes_a_ler_neste_bloco);
                        bytes_coletados += bytes_a_ler_neste_bloco;
                        p_atual += bytes_a_ler_neste_bloco;
                    } else {
                        send(sock, "ERRO 4 Falha ao obter bloco remoto", 36, 0);
                        free(resultado); free(fonte_dados); close(sock); return NULL;
                    }
                    free(fonte_dados);
                }
                send(sock, resultado, tam, 0);
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
            // ... (lógica de salvar, sem alterações significativas) ...
        } else if (strcmp(command, "ATUALIZAR_BLOCO") == 0) {
            // ... (lógica de atualizar, sem alterações significativas) ...
        } else if (strcmp(command, "INVALIDAR_BLOCO") == 0) {
            int id_bloco;
            sscanf(buffer_req, "%*s %d", &id_bloco);
            pthread_mutex_lock(&cache_mutex);
            for(int i = 0; i < tamanho_cache; i++){ // Invalidação na cache de tamanho limitado
                if(cache[i].id == id_bloco && cache[i].valido){
                    cache[i].valido = 0;
                    printf("[P%d] Cache para o bloco %d (slot %d) INVALIDADA.\n", my_rank, id_bloco, i);
                    break;
                }
            }
            pthread_mutex_unlock(&cache_mutex);
        } else {
             send(sock, "ERRO 3 Comando desconhecido", 28, 0);
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
    
    // --- Inicialização da Cache FIFO ---
    tamanho_cache = (int)(K_BLOCOS * 0.20);
    if (tamanho_cache == 0) tamanho_cache = 1; // Garante no mínimo 1 slot
    
    printf("[P%d] Tamanho da cache: %d blocos.\n", my_rank, tamanho_cache);
    cache = malloc(sizeof(BlocoCache) * tamanho_cache);
    for(int i = 0; i < tamanho_cache; i++){
        cache[i].id = -1; // ID inválido para indicar que o slot está vazio
        cache[i].dados = malloc(T_BLOCO);
        cache[i].valido = 0; 
    }
    pthread_mutex_init(&cache_mutex, NULL);

    int listening_socket, client_sock;
    struct sockaddr_in server, client;
    socklen_t c = sizeof(struct sockaddr_in);

    listening_socket = socket(AF_INET, SOCK_STREAM, 0);
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(BASE_PORT + my_rank);

    if (bind(listening_socket, (struct sockaddr*)&server, sizeof(server)) < 0) die("bind falhou");
    listen(listening_socket, MAX_CONEXOES);
    printf("[P%d] Escutando na porta %d...\n", my_rank, BASE_PORT + my_rank);

    while ((client_sock = accept(listening_socket, (struct sockaddr*)&client, &c))) {
        pthread_t sniffer_thread;
        int* new_sock = malloc(sizeof(int));
        *new_sock = client_sock;
        if (pthread_create(&sniffer_thread, NULL, handle_connection, (void*)new_sock) < 0) {
            perror("nao foi possivel criar a thread");
        }
    }
    
    return 0;
}