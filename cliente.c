// cliente.c
// Compilar com: gcc cliente.c -o cliente.exe -lws2_32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

// --- Definições ---
#define BASE_PORT 7000
#define MAX_BUFFER_SIZE 8192
#define COORDENADOR_RANK 0 // Processo 0 é o coordenador inicial

// Definição de byte para a API
typedef unsigned char byte;

// --- Códigos de Erro da API ---
#define SUCESSO 0
#define ERRO_CONEXAO -1
#define ERRO_MEMORIA_INEXISTENTE -2
#define ERRO_COMANDO_DESCONHECIDO -3
#define ERRO_FALHA_OBTER_BLOCO -4
#define ERRO_RESPOSTA_SERVIDOR -5

// --- Funções da API ---

// Função interna para se comunicar com um nó da rede
int comunicar_com_no(int no_id, const char* msg, int msg_len, char* resposta, int tam_resposta) {
    WSADATA wsa;
    SOCKET s;
    struct sockaddr_in server;

    WSAStartup(MAKEWORD(2,2),&wsa);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return ERRO_CONEXAO;

    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_family = AF_INET;
    server.sin_port = htons(BASE_PORT + no_id);

    if (connect(s, (struct sockaddr *)&server, sizeof(server)) < 0) {
        closesocket(s); WSACleanup(); return ERRO_CONEXAO;
    }
    
    send(s, msg, msg_len, 0);
    
    int recv_size = recv(s, resposta, tam_resposta, 0);
    if (recv_size > 0) resposta[recv_size] = '\0';
    
    closesocket(s);
    WSACleanup();

    if (strncmp(resposta, "ERRO", 4) == 0) {
        int cod_erro;
        sscanf(resposta, "ERRO %d", &cod_erro);
        fprintf(stderr, "API: O servidor retornou um erro -> %s\n", resposta);
        return -cod_erro; // Retorna o código de erro negativo
    }
    return SUCESSO;
}

// Função de leitura da memória compartilhada
int le(int posicao, byte *buffer, int tamanho) {
    char msg[100];
    char resposta[MAX_BUFFER_SIZE];
    sprintf(msg, "OBTER_DADOS %d %d", posicao, tamanho);

    int status = comunicar_com_no(COORDENADOR_RANK, msg, strlen(msg), resposta, tamanho);
    if (status == SUCESSO) {
        memcpy(buffer, resposta, tamanho);
    }
    return status;
}

// Função de escrita na memória compartilhada
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


// --- Main com Casos de Teste ---
void run_test(const char* test_name, int status, int expected_status) {
    printf("   -> Resultado: ");
    if (status == expected_status) {
        printf("PASSOU (Retorno %d)\n", status);
    } else {
        printf("FALHOU (Esperado: %d, Recebido: %d)\n", expected_status, status);
    }
}

int main(int argc, char *argv[]) {
    // Parâmetros do sistema para os testes. Devem ser os mesmos usados para iniciar o servidor.
    int k_blocos = 4;
    int t_bloco = 10;
    int memoria_total = k_blocos * t_bloco;

    printf("--- INICIANDO TESTES DO CLIENTE DSM ---\n");
    printf("Configuracao esperada do servidor: 2 processos, %d blocos, %d bytes/bloco\n", k_blocos, t_bloco);
    printf("----------------------------------------\n\n");

    // --- Teste 1: Escrita e Leitura que abrange múltiplos processos ---
    // Com 2 processos e 4 blocos, P0 tem blocos 0,1 (pos 0-19) e P1 tem 2,3 (pos 20-39)
    // Uma escrita na posição 15 de 10 bytes vai atingir o bloco 1 (P0) e o bloco 2 (P1)
    printf("--- Teste 1: Escrita/Leitura em blocos de processos distintos ---\n");
    char* dados_teste1 = "DADO-SPLIT";
    int pos1 = 15, tam1 = strlen(dados_teste1);
    printf("1.1. Escrevendo '%s' na posicao %d (atinge bloco 1 do P0 e bloco 2 do P1)\n", dados_teste1, pos1);
    int status1 = escreve(pos1, (byte*)dados_teste1, tam1);
    run_test("Escrita Multi-Bloco", status1, SUCESSO);
    sleep(1); // Pausa para servidores processarem

    printf("1.2. Lendo de volta da posicao %d para verificar a escrita\n", pos1);
    byte buffer1[50] = {0};
    status1 = le(pos1, buffer1, tam1);
    run_test("Leitura Multi-Bloco", status1, SUCESSO);
    if(status1 == SUCESSO) {
        printf("   -> Dados lidos: '%s'. Verificacao: %s\n", buffer1, strcmp((char*)buffer1, dados_teste1) == 0 ? "OK" : "FALHOU");
    }
    printf("\n");

    // --- Teste 2: Invalidação de Cache ---
    printf("--- Teste 2: Invalidação de Cache ---\n");
    // P0 coordena, P1 tem o bloco 3 (pos 30-39)
    int pos2 = 32, tam2 = 5;
    char* dados_originais = "UNIV-";
    char* dados_novos = "UFSC--";
    byte buffer2[50] = {0};

    printf("2.1. Lendo dados do bloco 3 (P1) pela 1a vez para popular a cache do P0.\n");
    escreve(pos2, (byte*)dados_originais, tam2); sleep(1);
    status1 = le(pos2, buffer2, tam2);
    run_test("Leitura para cache", status1, SUCESSO);
    if(status1 == SUCESSO) printf("   -> Dados lidos: '%s'\n", buffer2);
    
    printf("2.2. Escrevendo NOVOS dados no bloco 3 (isso deve invalidar a cache do P0).\n");
    status1 = escreve(pos2, (byte*)dados_novos, strlen(dados_novos));
    run_test("Escrita para invalidacao", status1, SUCESSO);
    sleep(1);

    printf("2.3. Lendo dados do bloco 3 novamente. P0 deve ter um cache miss e buscar o novo valor.\n");
    status1 = le(pos2, buffer2, strlen(dados_novos));
    run_test("Leitura pos-invalidacao", status1, SUCESSO);
    if(status1 == SUCESSO) {
        printf("   -> Dados lidos: '%s'. Verificacao: %s\n", buffer2, strcmp((char*)buffer2, dados_novos) == 0 ? "OK" : "FALHOU");
    }
    printf("\n");

    // --- Teste 3: Acesso a memória fora dos limites ---
    printf("--- Teste 3: Acesso a memoria fora dos limites ---\n");
    int pos3 = memoria_total + 5; // Posição inválida
    printf("3.1. Tentando ler %d bytes da posicao %d (invalida).\n", tam1, pos3);
    status1 = le(pos3, buffer1, tam1);
    run_test("Leitura fora do limite", status1, ERRO_MEMORIA_INEXISTENTE);
    printf("\n");

    printf("--- FIM DOS TESTES ---\n");
    return 0;
}