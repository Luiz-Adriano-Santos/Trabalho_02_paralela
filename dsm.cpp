#include "dsm.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>

// Construtor: inicializa o nó, lê a configuração e aloca a memória local.
DsmNode::DsmNode(int id, const std::string& config_file) : my_id(id) {
    parse_config(config_file);

    // Determina o número de blocos que este nó possui
    for (size_t i = 0; i < num_blocks; ++i) {
        if (get_owner(i) == my_id) {
            local_blocks.emplace_back(std::vector<char>(BLOCK_SIZE, 0));
        }
    }
    std::cout << "[Node " << my_id << "] Initialized. Owns " << local_blocks.size() << " blocks." << std::endl;
}

DsmNode::~DsmNode() {
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

// Inicia a thread que executará o loop do servidor.
void DsmNode::start_server() {
    server_thread = std::thread(&DsmNode::server_loop, this);
}

// Lê o arquivo de configuração para obter informações dos outros nós.
void DsmNode::parse_config(const std::string& config_file) {
    std::ifstream file(config_file);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open config file: " + config_file);
    }
    file >> num_processes >> num_blocks;
    nodes.resize(num_processes);
    for (int i = 0; i < num_processes; ++i) {
        file >> nodes[i].id >> nodes[i].ip >> nodes[i].port;
    }
}

// Determina o dono de um bloco usando uma função de hash simples.
int DsmNode::get_owner(int block_id) {
    return block_id % num_processes;
}

// API de Leitura
int DsmNode::le(int posicao, char* buffer, int tamanho) {
    int start_block = posicao / BLOCK_SIZE;
    int end_block = (posicao + tamanho - 1) / BLOCK_SIZE;

    for (int block_id = start_block; block_id <= end_block; ++block_id) {
        int owner_id = get_owner(block_id);
        const char* block_data_ptr;
        std::vector<char> temp_buffer; // Para dados buscados remotamente

        if (owner_id == my_id) {
            // Acesso local
            std::lock_guard<std::mutex> lock(local_blocks_mutex);
            int local_block_idx = block_id / num_processes;
            block_data_ptr = local_blocks[local_block_idx].data();
            std::cout << "[Node " << my_id << "] Reading local block " << block_id << std::endl;

        } else {
            // Acesso remoto
            std::unique_lock<std::mutex> lock(cache_mutex);
            if (cache.count(block_id) && cache[block_id].is_valid) {
                // Cache hit
                block_data_ptr = cache[block_id].data.data();
                std::cout << "[Node " << my_id << "] Reading remote block " << block_id << " from cache." << std::endl;
            } else {
                // Cache miss
                std::cout << "[Node " << my_id << "] Cache miss for block " << block_id << ". Fetching from owner " << owner_id << std::endl;
                lock.unlock(); // Desbloqueia antes de operação de rede

                int sock = connect_to_node(owner_id);
                if (sock < 0) return -1;

                MessageHeader req_header = {REQUEST_BLOCK, block_id, my_id, 0};
                send_full_message(sock, req_header, nullptr);

                MessageHeader res_header;
                std::vector<char> received_data;
                if (!read_full_message(sock, res_header, received_data) || res_header.type != RESPONSE_BLOCK) {
                    close(sock);
                    return -1;
                }
                close(sock);

                lock.lock(); // Rebloqueia para atualizar cache
                cache[block_id] = {received_data, true};
                temp_buffer = received_data;
                block_data_ptr = temp_buffer.data();
            }
        }

        // Copia a porção necessária do bloco para o buffer do usuário
        int pos_in_block = posicao % BLOCK_SIZE;
        int bytes_to_copy_from_block = std::min(tamanho, BLOCK_SIZE - pos_in_block);
        memcpy(buffer, block_data_ptr + pos_in_block, bytes_to_copy_from_block);

        buffer += bytes_to_copy_from_block;
        tamanho -= bytes_to_copy_from_block;
        posicao += bytes_to_copy_from_block;
    }
    return 0;
}

// API de Escrita
int DsmNode::escreve(int posicao, const char* buffer, int tamanho) {
    int start_block = posicao / BLOCK_SIZE;
    int end_block = (posicao + tamanho - 1) / BLOCK_SIZE;
    
    for (int block_id = start_block; block_id <= end_block; ++block_id) {
        int owner_id = get_owner(block_id);
        int pos_in_block = posicao % BLOCK_SIZE;
        int bytes_to_write_in_block = std::min(tamanho, BLOCK_SIZE - pos_in_block);
        
        // Prepara um buffer com o bloco completo a ser escrito
        std::vector<char> full_block_data(BLOCK_SIZE);
        if(bytes_to_write_in_block < BLOCK_SIZE) {
            // Escrita parcial: precisa ler o bloco antigo primeiro
            char temp_read_buffer[BLOCK_SIZE];
            // Usa a própria API 'le' para garantir que temos o bloco mais atual
            if (le(block_id * BLOCK_SIZE, temp_read_buffer, BLOCK_SIZE) != 0) {
                 std::cerr << "Failed to read block before partial write." << std::endl;
                 return -1;
            }
            memcpy(full_block_data.data(), temp_read_buffer, BLOCK_SIZE);
        }
        memcpy(full_block_data.data() + pos_in_block, buffer, bytes_to_write_in_block);


        if (owner_id == my_id) {
            // Escrita local
            std::cout << "[Node " << my_id << "] Writing to local block " << block_id << "." << std::endl;
            {
                std::lock_guard<std::mutex> lock(local_blocks_mutex);
                int local_block_idx = block_id / num_processes;
                local_blocks[local_block_idx] = full_block_data;
            }
            // Invalida as cópias em outros nós
            invalidate_copies(block_id);

        } else {
            // Escrita remota
            std::cout << "[Node " << my_id << "] Writing to remote block " << block_id << " on owner " << owner_id << std::endl;
            int sock = connect_to_node(owner_id);
            if (sock < 0) return -1;

            MessageHeader header = {WRITE_BLOCK, block_id, my_id, (size_t)full_block_data.size()};
            send_full_message(sock, header, full_block_data.data());

            // Espera confirmação
            MessageHeader res_header;
            std::vector<char> temp_data;
            if(!read_full_message(sock, res_header, temp_data) || res_header.type != WRITE_SUCCESS) {
                std::cerr << "Write to node " << owner_id << " not confirmed." << std::endl;
            }
            close(sock);
        }
        
        // Atualiza ponteiros para a próxima iteração do loop
        buffer += bytes_to_write_in_block;
        tamanho -= bytes_to_write_in_block;
        posicao += bytes_to_write_in_block;
    }
    return 0;
}