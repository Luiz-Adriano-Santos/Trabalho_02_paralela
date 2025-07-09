// cliente.c (Versão para Mac/Linux com Menu Interativo)
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

#define SUCESSO 0
#define ERRO_CONEXAO -1
#define ERRO_MEMORIA_INEXISTENTE -2
#define ERRO_COMANDO_DESCONHECIDO -3
#define ERRO_FALHA_OBTER_BLOCO -4
#define ERRO_RESPOSTA_SERVIDOR -5

// --- Funções da API (sem alterações na lógica) ---
int comunicar_com_no(int no_id, const char* msg, int msg_len, char* resposta, int tam_resposta) {
    int s;
    struct sockaddr_in server;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return ERRO_CONEXAO;
    
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_family = AF_INET;
    server.sin_port = htons(BASE_PORT + no_id);

    if (connect(s, (struct sockaddr *)&server, sizeof(server)) < 0) {
        close(s); return ERRO_CONEXAO;
    }
    send(s, msg, msg_len, 0);
    int recv_size = recv(s, resposta, tam_resposta, 0);
    if (recv_size > 0) resposta[recv_size] = '\0';
    close(s);

    if (strncmp(resposta, "ERRO", 4) == 0) {
        int cod_erro;
        sscanf(resposta, "ERRO %d", &cod_erro);
        fprintf(stderr, "API: O servidor retornou um erro -> %s\n", resposta);
        return -cod_erro;
    }
    return SUCESSO;
}

int le(int posicao, byte *buffer, int tamanho) {
    char msg[100];
    char resposta[MAX_BUFFER_SIZE];
    sprintf(msg, "OBTER_DADOS %d %d", posicao, tamanho);
    int status = comunicar_com_no(COORDENADOR_RANK, msg, strlen(msg), resposta, tamanho);
    if (status == SUCESSO) memcpy(buffer, resposta, tamanho);
    return status;
}

int escreve(int posicao, byte *buffer, int tamanho) {
    char msg[MAX_BUFFER_SIZE];
    char resposta[100];
    int len_cabecalho = sprintf(msg, "SALVAR_DADOS %d %d ", posicao, tamanho);
    memcpy(msg + len_cabecalho, buffer, tamanho);
    int status = comunicar_com_no(COORDENADOR_RANK, msg, len_cabecalho + tamanho, resposta, 100);
    if (status == SUCESSO && strncmp(resposta, "OK", 2) != 0) {
        fprintf(stderr, "API: Resposta inesperada do servidor: %s\n", resposta);
        return ERRO_RESPOSTA_SERVIDOR;
    }
    return status;
}

// --- Funções de Teste ---
void run_test(const char* test_name, int status, int expected_status) {
    printf("   -> Resultado: ");
    if (status == expected_status) printf("PASSOU (Retorno %d)\n", status);
    else printf("FALHOU (Esperado: %d, Recebido: %d)\n", expected_status, status);
}

void teste_leitura_escrita_multibloco() {
    printf("\n--- Teste 1: Escrita/Leitura em blocos de processos distintos ---\n");
    // Cenário: 4 processos, 10 blocos de 8 bytes.
    // P0: 0,1 (0-15), P1: 2,3 (16-31), P2: 4,5 (32-47), P3: 6,7,8,9 (48-79)
    // Escrita na pos 14 por 5 bytes atinge bloco 1 (P0) e 2 (P1).
    char* dados_teste1 = "SPLIT";
    int pos1 = 14, tam1 = strlen(dados_teste1);
    printf("1.1. Escrevendo '%s' na posicao %d (atinge bloco 1 do P0 e bloco 2 do P1)\n", dados_teste1, pos1);
    int status1 = escreve(pos1, (byte*)dados_teste1, tam1);
    run_test("Escrita Multi-Bloco", status1, SUCESSO);
    sleep(1);

    printf("1.2. Lendo de volta da posicao %d para verificar a escrita\n", pos1);
    byte buffer1[50] = {0};
    status1 = le(pos1, buffer1, tam1);
    run_test("Leitura Multi-Bloco", status1, SUCESSO);
    if(status1 == SUCESSO) {
        printf("   -> Dados lidos: '%s'. Verificacao: %s\n", buffer1, strcmp((char*)buffer1, dados_teste1) == 0 ? "OK" : "FALHOU");
    }
}

void teste_invalidacao_cache() {
    printf("\n--- Teste 2: Invalidação de Cache (Política FIFO) ---\n");
    // Cenário: P0 coordena. Cache de P0 tem 2 slots.
    // Bloco 3 (P1), Bloco 5 (P2), Bloco 7 (P3)
    char* dados_originais = "DADO-A";
    char* dados_novos = "DADO-B";
    char* dados_terceiro = "DADO-C";
    byte buffer[50] = {0};

    printf("2.1. Lendo Bloco 3 (P1) para popular o slot 0 da cache do P0.\n");
    le(17, buffer, 1); // pos 17 está no bloco 2 (P1)
    
    printf("2.2. Lendo Bloco 5 (P2) para popular o slot 1 da cache do P0.\n");
    le(33, buffer, 1); // pos 33 está no bloco 4 (P2)
    sleep(1);
    
    printf("2.3. Escrevendo NOVOS dados no Bloco 3 (P1). Isso deve invalidar o slot 0 da cache do P0.\n");
    escreve(17, (byte*)dados_novos, strlen(dados_novos));
    run_test("Escrita para invalidacao", SUCESSO, SUCESSO);
    sleep(1);

    printf("2.4. Lendo Bloco 3 (P1) novamente. P0 deve ter um cache miss e buscar o novo valor.\n");
    le(17, buffer, strlen(dados_novos));
    if (SUCESSO == 0) printf("   -> Dados lidos: '%s'. Verificacao: %s\n", buffer, strcmp((char*)buffer, dados_novos) == 0 ? "OK" : "FALHOU");

    printf("2.5. Lendo Bloco 7 (P3). Isso deve sobrescrever o slot mais antigo da cache (slot 0, que continha Bloco 5).\n");
    escreve(50, (byte*)dados_terceiro, strlen(dados_terceiro)); sleep(1);
    le(50, buffer, strlen(dados_terceiro));
    run_test("Leitura para eviccao FIFO", SUCESSO, SUCESSO);
}

void teste_limites_memoria() {
    printf("\n--- Teste 3: Acesso a memoria fora dos limites ---\n");
    int memoria_total = 10 * 8; // K_BLOCOS * T_BLOCO
    int pos3 = memoria_total + 5;
    byte buffer[10];
    printf("3.1. Tentando ler da posicao %d (invalida).\n", pos3);
    int status = le(pos3, buffer, 10);
    run_test("Leitura fora do limite", status, ERRO_MEMORIA_INEXISTENTE);
}

// --- Menu Principal ---
int main(int argc, char *argv[]) {
    int escolha = -1;
    char buffer_entrada[10];

    while (1) {
        printf("\n--- MENU DE TESTES DO CLIENTE DSM ---\n");
        printf("Configuracao esperada do servidor: 4 processos, 10 blocos, 8 bytes/bloco\n");
        printf("1. Teste de Leitura/Escrita em Múltiplos Blocos\n");
        printf("2. Teste de Invalidação e Substituição de Cache (FIFO)\n");
        printf("3. Teste de Acesso a Memória Fora dos Limites\n");
        printf("0. Sair\n");
        printf("Escolha uma opcao: ");

        if (fgets(buffer_entrada, sizeof(buffer_entrada), stdin) != NULL) {
            if (sscanf(buffer_entrada, "%d", &escolha) != 1) {
                escolha = -1; // Garante que a entrada inválida não seja processada
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
                printf("Opcao invalida! Por favor, escolha um numero de 0 a 3.\n");
                break;
        }
    }

    return 0;
}