#include "dsm.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#include <stdexcept>
#include <cstring>

// Função para estabelecer uma conexão com um nó.
int DsmNode::connect_to_node(int node_id) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(nodes[node_id].port);
    inet_pton(AF_INET, nodes[node_id].ip.c_str(), &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect failed");
        close(sock);
        return -1;
    }
    return sock;
}

// Loop principal do servidor, executado em uma thread separada.
void DsmNode::server_loop() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) throw std::runtime_error("Server socket creation failed");

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(nodes[my_id].port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        throw std::runtime_error("Server bind failed");
    }

    if (listen(server_fd, 10) < 0) {
        throw std::runtime_error("Server listen failed");
    }
    
    std::vector<struct pollfd> fds;
    fds.push_back({server_fd, POLLIN, 0});

    std::cout << "[Node " << my_id << "] Server listening on port " << nodes[my_id].port << std::endl;

    while (true) {
        int ret = poll(fds.data(), fds.size(), -1); // Bloqueia indefinidamente
        if (ret < 0) {
            perror("poll error");
            continue;
        }

        // Verifica se há uma nova conexão
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int new_sock = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (new_sock >= 0) {
                fds.push_back({new_sock, POLLIN, 0});
                 std::cout << "[Node " << my_id << "] New connection accepted." << std::endl;
            }
        }
        
        // Verifica os sockets dos clientes
        for (size_t i = 1; i < fds.size(); ++i) {
            if (fds[i].revents & POLLIN) {
                handle_connection(fds[i].fd);
                close(fds[i].fd);
                fds.erase(fds.begin() + i);
                i--; // Ajusta o índice após a remoção
            }
        }
    }
    close(server_fd);
}

// Lida com uma requisição de um cliente conectado.
void DsmNode::handle_connection(int client_sock) {
    MessageHeader header;
    std::vector<char> data;

    if (!read_full_message(client_sock, header, data)) return;

    switch (header.type) {
        case REQUEST_BLOCK: {
            std::cout << "[Node " << my_id << "] Received REQUEST for block " << header.block_id << " from " << header.source_id << std::endl;
            int local_block_idx = header.block_id / num_processes;
            const char* block_data;
            {
                std::lock_guard<std::mutex> lock(local_blocks_mutex);
                block_data = local_blocks[local_block_idx].data();
            }
            MessageHeader res_header = {RESPONSE_BLOCK, header.block_id, my_id, BLOCK_SIZE};
            send_full_message(client_sock, res_header, block_data);

            {
                std::lock_guard<std::mutex> lock(copy_holders_mutex);
                copy_holders[header.block_id].push_back(header.source_id);
            }
            break;
        }
        case WRITE_BLOCK: {
             std::cout << "[Node " << my_id << "] Received WRITE for block " << header.block_id << std::endl;
            int local_block_idx = header.block_id / num_processes;
            {
                std::lock_guard<std::mutex> lock(local_blocks_mutex);
                memcpy(local_blocks[local_block_idx].data(), data.data(), BLOCK_SIZE);
            }
            // Invalida cópias em outros nós e responde ao solicitante
            invalidate_copies(header.block_id);
            MessageHeader res_header = {WRITE_SUCCESS, header.block_id, my_id, 0};
            send_full_message(client_sock, res_header, nullptr);
            break;
        }
        case INVALIDATE_BLOCK: {
            std::cout << "[Node " << my_id << "] Received INVALIDATE for block " << header.block_id << std::endl;
            std::lock_guard<std::mutex> lock(cache_mutex);
            if (cache.count(header.block_id)) {
                cache[header.block_id].is_valid = false;
            }
            break;
        }
        default:
            break;
    }
}

// Envia mensagem de invalidação para todos os nós que possuem cópia de um bloco.
void DsmNode::invalidate_copies(int block_id) {
    std::lock_guard<std::mutex> lock(copy_holders_mutex);
    if (copy_holders.count(block_id)) {
        std::cout << "[Node " << my_id << "] Invalidating copies of block " << block_id << std::endl;
        MessageHeader header = {INVALIDATE_BLOCK, block_id, my_id, 0};
        for (int node_id : copy_holders[block_id]) {
            int sock = connect_to_node(node_id);
            if (sock >= 0) {
                send_full_message(sock, header, nullptr);
                close(sock);
            }
        }
        copy_holders.erase(block_id); // Limpa a lista após a invalidação
    }
}


// Funções auxiliares para garantir que a mensagem inteira seja enviada/recebida.
void DsmNode::send_full_message(int sock, const MessageHeader& header, const char* data) {
    if (write(sock, &header, sizeof(header)) != sizeof(header)) {
        perror("write header failed");
    }
    if (data && header.data_size > 0) {
        if (write(sock, data, header.data_size) != header.data_size) {
            perror("write data failed");
        }
    }
}

bool DsmNode::read_full_message(int sock, MessageHeader& header, std::vector<char>& data) {
    if (read(sock, &header, sizeof(header)) != sizeof(header)) {
        return false;
    }
    if (header.data_size > 0) {
        data.resize(header.data_size);
        if (read(sock, data.data(), header.data_size) != header.data_size) {
            return false;
        }
    }
    return true;
}