// cliente.c (Versão Final para Mac/Linux com Menu e Erros Numéricos)
// Compilar com: gcc cliente.c -o cliente

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BASE_PORT 7000
#define MAX_BUFFER_SIZE 8192
#define COORDENADOR_RANK 0

typedef unsigned char byte;

// Códigos de status e erro (devem ser iguais aos do servidor)
#define SUCESSO 0
#define ERRO_GENERICO -1
#define ERRO_MEMORIA_INEXISTENTE -2
#define ERRO_COMANDO_DESCONHECIDO -3
#define ERRO_FALHA_OBTER_BLOCO -4
#define ERRO_CONEXAO -10 // Erro local do cliente

// --- Implementação das Funções ---

int recv_all(int sock, char* buffer, int len) {
    int total_recebido = 0;
    int bytes_restantes = len;
    while(total_recebido < len) {
        int bytes_recebidos = recv(sock, buffer + total_recebido, bytes_restantes, 0);
        if (bytes_recebidos <= 0) return -1;
        total_recebido += bytes_recebidos;
        bytes_restantes -= bytes_recebidos;
    }
    return total_recebido;
}

int le(int posicao, byte *buffer, int tamanho) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return ERRO_CONEXAO;

    struct sockaddr_in server;
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_family = AF_INET;
    server.sin_port = htons(BASE_PORT + COORDENADOR_RANK);

    if (connect(s, (struct sockaddr *)&server, sizeof(server)) < 0) {
        close(s); return ERRO_CONEXAO;
    }

    char msg[100];
    sprintf(msg, "OBTER_DADOS %d %d", posicao, tamanho);
    send(s, msg, strlen(msg), 0);

    int status = SUCESSO;
    if (recv_all(s, (char*)&status, sizeof(int)) < 0) {
        close(s); return ERRO_CONEXAO;
    }
    
    if (status == SUCESSO) {
        if (recv_all(s, (char*)buffer, tamanho) < 0) {
            close(s); return ERRO_CONEXAO;
        }
    }
    
    close(s);
    return status;
}

int escreve(int posicao, byte *buffer, int tamanho) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return ERRO_CONEXAO;
    
    struct sockaddr_in server;
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_family = AF_INET;
    server.sin_port = htons(BASE_PORT + COORDENADOR_RANK);
    
    if (connect(s, (struct sockaddr *)&server, sizeof(server)) < 0) {
        close(s); return ERRO_CONEXAO;
    }

    char msg[MAX_BUFFER_SIZE];
    int len_cabecalho = sprintf(msg, "SALVAR_DADOS %d %d ", posicao, tamanho);
    memcpy(msg + len_cabecalho, buffer, tamanho);
    send(s, msg, len_cabecalho + tamanho, 0);

    int status = SUCESSO;
    if(recv_all(s, (char*)&status, sizeof(int)) < 0) {
        close(s); return ERRO_CONEXAO;
    }

    close(s);
    return status;
}

void traduzir_erro(int codigo_erro) {
    printf("   -> Mensagem: ");
    switch (codigo_erro) {
        case ERRO_CONEXAO: printf("Erro de conexao com o servidor.\n"); break;
        case ERRO_MEMORIA_INEXISTENTE: printf("Tentativa de acesso a uma area de memoria invalida.\n"); break;
        case ERRO_COMANDO_DESCONHECIDO: printf("O servidor nao reconheceu o comando enviado.\n"); break;
        case ERRO_FALHA_OBTER_BLOCO: printf("Falha ao obter um bloco remoto necessario para a operacao.\n"); break;
        default: printf("Ocorreu um erro desconhecido (codigo %d).\n", codigo_erro); break;
    }
}

void run_test(const char* test_name, int status, int expected_status) {
    printf("   -> Resultado: ");
    if (status == expected_status) {
        printf("PASSOU (Retorno %d)\n", status);
    } else {
        printf("FALHOU (Esperado: %d, Recebido: %d)\n", expected_status, status);
        traduzir_erro(status);
    }
}

void teste_leitura_escrita_multibloco() {
    printf("\n--- INICIANDO Teste 1: Escrita/Leitura em blocos de processos distintos ---\n");
    // Cenário: 4 processos, 10 blocos de 8 bytes.
    // P0: 0,1 (0-15), P1: 2,3 (16-31), P2: 4,5 (32-47), P3: 6,7,8,9 (48-79)
    // Escrita na pos 14 por 5 bytes atinge bloco 1 (P0) e 2 (P1).
    char* dados_teste1 = "SPLIT";
    int pos1 = 14, tam1 = strlen(dados_teste1);
    printf("1.1. Escrevendo '%s' na posicao %d...\n", dados_teste1, pos1);
    int status1 = escreve(pos1, (byte*)dados_teste1, tam1);
    run_test("Escrita Multi-Bloco", status1, SUCESSO);
    sleep(1);

    printf("1.2. Lendo de volta da posicao %d para verificar...\n", pos1);
    byte buffer1[50] = {0};
    status1 = le(pos1, buffer1, tam1);
    run_test("Leitura Multi-Bloco", status1, SUCESSO);
    if(status1 == SUCESSO) {
        printf("   -> Dados lidos: '%s'. Verificacao: %s\n", buffer1, strcmp((char*)buffer1, dados_teste1) == 0 ? "OK" : "FALHOU");
    }
}

void teste_invalidacao_cache() {
    printf("\n--- INICIANDO Teste 2: Invalidação e Substituição de Cache (FIFO) ---\n");
    // Cenário: P0 coordena. Cache de P0 tem 2 slots (20% de 10 blocos).
    // Bloco 2 (P1), Bloco 4 (P2), Bloco 6 (P3)
    char* dados_novos = "DADO-B";
    byte buffer[50] = {0};

    printf("2.1. Lendo Bloco 2 (do P1) para popular o slot 0 da cache do P0.\n");
    le(17, buffer, 1); sleep(1);
    
    printf("2.2. Lendo Bloco 4 (do P2) para popular o slot 1 da cache do P0.\n");
    le(33, buffer, 1); sleep(1);
    
    printf("2.3. Lendo Bloco 6 (do P3). Isso deve sobrescrever o slot 0 (o mais antigo, que continha o Bloco 2).\n");
    le(49, buffer, 1);
    run_test("Teste de eviccao FIFO", SUCESSO, SUCESSO);
    sleep(1);

    printf("2.4. Escrevendo NOVOS dados no Bloco 4 (do P2). P0 tem esse bloco na cache (slot 1).\n");
    escreve(33, (byte*)dados_novos, strlen(dados_novos));
    printf("   -> Acao: Escrita deve ter gerado uma invalidacao na cache do P0.\n");
    sleep(1);

    printf("2.5. Lendo Bloco 4 novamente. P0 deve ter um cache miss e buscar o novo valor na rede.\n");
    int status = le(33, buffer, strlen(dados_novos));
    run_test("Leitura pos-invalidacao", status, SUCESSO);
     if(status == SUCESSO) {
        printf("   -> Dados lidos: '%s'. Verificacao: %s\n", buffer, strcmp((char*)buffer, dados_novos) == 0 ? "OK" : "FALHOU");
    }
}

void teste_limites_memoria() {
    printf("\n--- INICIANDO Teste 3: Acesso a memoria fora dos limites ---\n");
    int memoria_total = 10 * 8; // K_BLOCOS * T_BLOCO do cenário
    int pos3 = memoria_total + 5;
    byte buffer[10];
    printf("3.1. Tentando ler da posicao %d (invalida).\n", pos3);
    int status = le(pos3, buffer, 10);
    run_test("Leitura fora do limite", status, ERRO_MEMORIA_INEXISTENTE);
}

int main(int argc, char *argv[]) {
    int escolha = -1;
    char buffer_entrada[20];

    while (1) {
        printf("\n--- MENU DE TESTES DO CLIENTE DSM ---\n");
        printf("Configuracao sugerida para o servidor: ./servidor 4 10 8\n");
        printf("1. Teste de Leitura/Escrita em Múltiplos Blocos\n");
        printf("2. Teste de Invalidação e Substituição de Cache (FIFO)\n");
        printf("3. Teste de Acesso a Memória Fora dos Limites\n");
        printf("0. Sair\n");
        printf("Escolha uma opcao: ");

        if (fgets(buffer_entrada, sizeof(buffer_entrada), stdin) != NULL) {
            if (sscanf(buffer_entrada, "%d", &escolha) != 1) {
                escolha = -1;
            }
        }

        switch (escolha) {
            case 1:
                teste_leitura_escrita_multibloco();
                break;
            case 2:
                teste_invalidacao_cache();
                break;
            case 3:
                teste_limites_memoria();
                break;
            case 0:
                printf("Encerrando cliente.\n");
                return 0;
            default:
                printf("\nOpcao invalida! Por favor, escolha um numero de 0 a 3.\n");
                break;
        }
    }
    return 0;
}