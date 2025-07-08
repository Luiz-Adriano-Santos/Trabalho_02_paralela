// servidor.c
// Compilar com: gcc servidor.c -o servidor.exe -lws2_32 -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <pthread.h>

#pragma comment(lib, "ws2_32.lib")

// --- Constantes e Definições ---
#define BASE_PORT 7000
#define MAX_BUFFER_SIZE 8192
#define MAX_CONEXOES 20

// Estrutura para representar um bloco de memória local
typedef struct {
    int id;      // Identificador global do bloco
    char* dados; // Ponteiro para os dados do bloco (tamanho T_BLOCO)
} BlocoMemoria;

// Estrutura para representar um bloco em cache
typedef struct {
    int id;      // Identificador global do bloco
    char* dados; // Cópia dos dados do bloco
    int valido;  // Flag: 1 se a cópia é válida, 0 se foi invalidada
} BlocoCache;

// --- Variáveis Globais ---
// Parâmetros do sistema, definidos na inicialização
int N_PROCESSOS = 0;
int K_BLOCOS = 0;
int T_BLOCO = 0;

// Identificador deste processo (0 para o pai, 1, 2, ... para os filhos)
int my_rank = 0;

// Ponteiros para os espaços de memória
BlocoMemoria* blocos_locais = NULL; // Array de blocos que este processo possui
int num_blocos_locais = 0;          // Quantidade de blocos em 'blocos_locais'
BlocoCache* cache = NULL;           // Array que representa a cache de blocos remotos

// Mutex para garantir acesso seguro à cache em ambiente com threads
pthread_mutex_t cache_mutex;

// --- Protótipos de Funções ---
void* handle_connection(void* socket_desc);
int obter_bloco_remoto(int id_bloco, char* buffer_retorno);

// --- Funções de Utilidade ---

// Função para terminar o programa em caso de erro fatal
void die(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

// Calcula qual processo (rank) é o "dono" de um determinado bloco
int calcular_dono(int id_bloco) {
    if (id_bloco < 0 || id_bloco >= K_BLOCOS) return -1; // ID de bloco inválido
    int blocos_por_processo = K_BLOCOS / N_PROCESSOS;
    int dono = id_bloco / blocos_por_processo;
    // O último processo fica com os blocos restantes se a divisão não for exata
    return (dono >= N_PROCESSOS) ? N_PROCESSOS - 1 : dono;
}

// Mapeia uma posição de memória global para um ID de bloco e um offset dentro do bloco
void mapear_posicao_global(int posicao, int* id_bloco, int* offset_bloco) {
    *id_bloco = posicao / T_BLOCO;
    *offset_bloco = posicao % T_BLOCO;
}

// --- Funções de Rede ---

// Função para enviar uma mensagem para um processo específico (não espera resposta)
void enviar_msg_assincrona(int rank_destino, const char* msg) {
    WSADATA wsa;
    SOCKET s;
    struct sockaddr_in server;
    
    WSAStartup(MAKEWORD(2,2),&wsa);
    s = socket(AF_INET, SOCK_STREAM, 0);
    
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_family = AF_INET;
    server.sin_port = htons(BASE_PORT + rank_destino);

    if (connect(s, (struct sockaddr *)&server, sizeof(server)) >= 0) {
        send(s, msg, strlen(msg), 0);
    }
    closesocket(s);
    WSACleanup();
}

// A função principal que roda em uma thread para cada nova conexão
void* handle_connection(void* socket_desc) {
    int sock = *(int*)socket_desc;
    free(socket_desc);
    char buffer_req[MAX_BUFFER_SIZE];
    int read_size;

    memset(buffer_req, 0, MAX_BUFFER_SIZE);
    // Lê a mensagem da conexão
    if ((read_size = recv(sock, buffer_req, MAX_BUFFER_SIZE, 0)) > 0) {
        buffer_req[read_size] = '\0';
        printf("[P%d] Recebeu: '%.50s...'\n", my_rank, buffer_req);

        char command[30];
        sscanf(buffer_req, "%s", command);

        // --- Lógica do Protocolo ---

        if (strcmp(command, "OBTER_DADOS") == 0) {
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

                    if (dono == my_rank) { // O bloco é local
                        for(int i = 0; i < num_blocos_locais; i++) {
                            if (blocos_locais[i].id == id_bloco) {
                                memcpy(fonte_dados, blocos_locais[i].dados, T_BLOCO);
                                sucesso_leitura = 1;
                                printf("[P%d] Acesso LOCAL ao bloco %d.\n", my_rank, id_bloco);
                                break;
                            }
                        }
                    } else { // O bloco é remoto, verificar a cache
                        pthread_mutex_lock(&cache_mutex);
                        for(int i = 0; i < K_BLOCOS; i++){
                            if(cache[i].id == id_bloco && cache[i].valido){
                                memcpy(fonte_dados, cache[i].dados, T_BLOCO);
                                sucesso_leitura = 1;
                                printf("[P%d] Acesso via CACHE ao bloco %d.\n", my_rank, id_bloco);
                                break;
                            }
                        }
                        pthread_mutex_unlock(&cache_mutex);

                        if (!sucesso_leitura) { // Cache miss, buscar remotamente
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
                        free(resultado);
                        free(fonte_dados);
                        closesocket(sock);
                        return NULL;
                    }
                    free(fonte_dados);
                }
                send(sock, resultado, tam, 0);
                free(resultado);
            }
        } else if (strcmp(command, "OBTER_BLOCO_INTERNO") == 0) { // Requisição interna de outro processo
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
            char* dados_a_salvar = buffer_req + 13; // Pula "SALVAR_DADOS "
            sscanf(buffer_req, "%*s %d %d", &pos, &tam);
            
            if (pos < 0 || (pos + tam) > (K_BLOCOS * T_BLOCO)) {
                send(sock, "ERRO 2 Memoria fora dos limites", 32, 0);
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
                send(sock, "OK", 2, 0);
            }
        } else if (strcmp(command, "ATUALIZAR_BLOCO") == 0) {
            int id_bloco, offset, tam;
            sscanf(buffer_req, "%*s %d %d %d", &id_bloco, &offset, &tam);
            char* dados_recebidos = strstr(buffer_req, " ") + 1; // Encontra o primeiro espaço
            dados_recebidos = strstr(dados_recebidos, " ") + 1; // Encontra o segundo
            dados_recebidos = strstr(dados_recebidos, " ") + 1; // Encontra o terceiro

            // Atualiza o bloco local
            for(int i = 0; i < num_blocos_locais; i++){
                if(blocos_locais[i].id == id_bloco){
                    memcpy(blocos_locais[i].dados + offset, dados_recebidos, tam);
                    printf("[P%d] Bloco LOCAL %d atualizado.\n", my_rank, id_bloco);
                    
                    // Envia mensagem de invalidação para todos os outros processos
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
            for(int i = 0; i < K_BLOCOS; i++){
                if(cache[i].id == id_bloco && cache[i].valido){
                    cache[i].valido = 0; // Invalida a cache
                    printf("[P%d] Cache para o bloco %d INVALIDADA.\n", my_rank, id_bloco);
                    break;
                }
            }
            pthread_mutex_unlock(&cache_mutex);
        } else {
             send(sock, "ERRO 3 Comando desconhecido", 28, 0);
        }
    }

    closesocket(sock);
    return NULL;
}

// Função síncrona para obter um bloco remoto e colocá-lo na cache
int obter_bloco_remoto(int id_bloco, char* buffer_retorno) {
    int dono = calcular_dono(id_bloco);
    char msg[100];
    sprintf(msg, "OBTER_BLOCO_INTERNO %d", id_bloco);
    
    WSADATA wsa;
    SOCKET s;
    struct sockaddr_in server;
    
    WSAStartup(MAKEWORD(2,2),&wsa);
    s = socket(AF_INET, SOCK_STREAM, 0);
    
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_family = AF_INET;
    server.sin_port = htons(BASE_PORT + dono);

    if (connect(s, (struct sockaddr *)&server, sizeof(server)) < 0) return -1;
    
    send(s, msg, strlen(msg), 0);
    int recv_size = recv(s, buffer_retorno, T_BLOCO, MSG_WAITALL); // Espera receber todo o bloco
    
    closesocket(s);
    WSACleanup();

    if (recv_size == T_BLOCO) {
        pthread_mutex_lock(&cache_mutex);
        for(int i=0; i<K_BLOCOS; i++){
            if(cache[i].id == id_bloco){
                memcpy(cache[i].dados, buffer_retorno, T_BLOCO);
                cache[i].valido = 1;
                printf("[P%d] Bloco %d obtido do P%d e ARMAZENADO NA CACHE.\n", my_rank, id_bloco, dono);
                break;
            }
        }
        pthread_mutex_unlock(&cache_mutex);
        return 0;
    }
    return -1;
}

// --- Função Principal ---
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <num_processos> <num_blocos> <tamanho_bloco>\n", argv[0]);
        exit(1);
    }

    N_PROCESSOS = atoi(argv[1]);
    K_BLOCOS = atoi(argv[2]);
    T_BLOCO = atoi(argv[3]);

    pid_t pids[N_PROCESSOS];

    // Cria os N-1 processos filhos
    for (int i = 0; i < N_PROCESSOS - 1; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            my_rank = i + 1;
            break; 
        }
    }

    printf("[P%d] Iniciado. PID: %d\n", my_rank, getpid());

    // --- Inicialização de Memória de cada processo ---
    int blocos_por_processo = K_BLOCOS / N_PROCESSOS;
    int blocos_inicio = my_rank * blocos_por_processo;
    int blocos_fim = (my_rank == N_PROCESSOS - 1) ? (K_BLOCOS - 1) : (blocos_inicio + blocos_por_processo - 1);
    num_blocos_locais = blocos_fim - blocos_inicio + 1;
    
    if (num_blocos_locais > 0) {
        blocos_locais = malloc(sizeof(BlocoMemoria) * num_blocos_locais);
        for(int i = 0; i < num_blocos_locais; i++){
            blocos_locais[i].id = blocos_inicio + i;
            blocos_locais[i].dados = malloc(T_BLOCO);
            memset(blocos_locais[i].dados, '-', T_BLOCO); // Inicializa com '-'
        }
    }
    printf("[P%d] Responsavel pelos blocos de %d a %d.\n", my_rank, blocos_inicio, blocos_fim);
    
    // Inicializa a cache (pode conter qualquer bloco)
    cache = malloc(sizeof(BlocoCache) * K_BLOCOS);
    for(int i = 0; i < K_BLOCOS; i++){
        cache[i].id = i;
        cache[i].dados = malloc(T_BLOCO);
        cache[i].valido = 0; 
    }
    pthread_mutex_init(&cache_mutex, NULL);

    // --- Inicialização da Rede (Winsock) ---
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2),&wsa);

    SOCKET listening_socket, client_sock;
    struct sockaddr_in server, client;
    int c = sizeof(struct sockaddr_in);

    listening_socket = socket(AF_INET, SOCK_STREAM, 0);
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(BASE_PORT + my_rank);

    bind(listening_socket, (struct sockaddr*)&server, sizeof(server));
    listen(listening_socket, MAX_CONEXOES);
    printf("[P%d] Escutando na porta %d...\n", my_rank, BASE_PORT + my_rank);

    // Loop para aceitar conexões e despachar para threads
    while ((client_sock = accept(listening_socket, (struct sockaddr*)&client, &c)) != INVALID_SOCKET) {
        pthread_t sniffer_thread;
        int* new_sock = malloc(sizeof(int));
        *new_sock = client_sock;
        pthread_create(&sniffer_thread, NULL, handle_connection, (void*)new_sock);
    }

    // Limpeza
    free(blocos_locais);
    free(cache);
    pthread_mutex_destroy(&cache_mutex);
    closesocket(listening_socket);
    WSACleanup();

    return 0;
}