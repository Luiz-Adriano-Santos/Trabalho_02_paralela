#ifndef DSM_H
#define DSM_H

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <arpa/inet.h>

// Tamanho fixo para os blocos de memória (t)
#define BLOCK_SIZE 4096

// Estrutura para armazenar informações de um nó (IP e porta)
struct NodeInfo {
    int id;
    std::string ip;
    int port;
};

// Estrutura para um bloco em cache
struct CachedBlock {
    std::vector<char> data;
    bool is_valid;
};

// Tipos de mensagem do nosso protocolo
enum MessageType {
    REQUEST_BLOCK,
    RESPONSE_BLOCK,
    WRITE_BLOCK,
    INVALIDATE_BLOCK,
    WRITE_SUCCESS, // Confirmação de escrita
    ERROR
};

// Cabeçalho das mensagens trocadas entre os nós
struct MessageHeader {
    MessageType type;
    int block_id;
    int source_id;
    size_t data_size;
};

class DsmNode {
public:
    DsmNode(int id, const std::string& config_file);
    ~DsmNode();

    // API pública para leitura e escrita
    int le(int posicao, char* buffer, int tamanho);
    int escreve(int posicao, const char* buffer, int tamanho);

    // Inicia a thread do servidor
    void start_server();

private:
    int my_id;
    int num_processes;
    size_t num_blocks;

    std::vector<NodeInfo> nodes;
    std::vector<std::vector<char>> local_blocks;
    std::unordered_map<int, CachedBlock> cache;
    std::unordered_map<int, std::vector<int>> copy_holders; // Dono do bloco -> [lista de quem tem cópia]

    std::mutex cache_mutex;
    std::mutex local_blocks_mutex;
    std::mutex copy_holders_mutex;

    std::thread server_thread;

    // Métodos privados
    void parse_config(const std::string& config_file);
    int get_owner(int block_id);
    void server_loop();

    // Funções auxiliares de rede
    int connect_to_node(int node_id);
    void send_full_message(int sock, const MessageHeader& header, const char* data);
    bool read_full_message(int sock, MessageHeader& header, std::vector<char>& data);
    void handle_connection(int client_sock);
    void invalidate_copies(int block_id);
};

#endif // DSM_H