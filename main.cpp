#include "dsm.h"
#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>

// Função de teste para demonstrar a funcionalidade
void run_test(DsmNode& node, int my_id, int num_processes) {
    // Dá um tempo para todos os servidores iniciarem
    sleep(2);

    // Teste 1: Escrita e Leitura
    if (my_id == 0) {
        std::string s = "Hello from Node 0!";
        std::cout << "\n[TEST] Node 0 writing '" << s << "' to position 10" << std::endl;
        node.escreve(10, s.c_str(), s.length() + 1);
    }

    sleep(1); // Sincronização simples

    if (my_id == 1) {
        char buffer[100] = {0};
        std::cout << "\n[TEST] Node 1 reading from position 10" << std::endl;
        node.le(10, buffer, 100);
        std::cout << "[TEST] Node 1 read: '" << buffer << "'" << std::endl;
    }

    sleep(1);

    // Teste 2: Cache Hit
    if (my_id == 1) {
        char buffer[100] = {0};
        std::cout << "\n[TEST] Node 1 reading again from position 10 (should be cached)" << std::endl;
        node.le(10, buffer, 100);
        std::cout << "[TEST] Node 1 read: '" << buffer << "'" << std::endl;
    }
    
    sleep(1);

    // Teste 3: Invalidação de Cache
    if (my_id == 0) {
        std::string s = "A new message!";
        std::cout << "\n[TEST] Node 0 writing a new message to position 10 (will invalidate Node 1's cache)" << std::endl;
        node.escreve(10, s.c_str(), s.length() + 1);
    }

    sleep(1);

    if (my_id == 1) {
        char buffer[100] = {0};
        std::cout << "\n[TEST] Node 1 reading again from position 10 (should fetch again)" << std::endl;
        node.le(10, buffer, 100);
        std::cout << "[TEST] Node 1 read: '" << buffer << "'" << std::endl;
    }
}


int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <my_id> <config_file>" << std::endl;
        return 1;
    }

    int my_id = std::stoi(argv[1]);
    std::string config_file = argv[2];

    try {
        DsmNode node(my_id, config_file);
        node.start_server();

        // Executa a rotina de teste
        run_test(node, my_id, 2); // Assumindo teste com 2 processos

        // Mantém o programa principal vivo para que o servidor continue rodando
        std::cout << "\n[Node " << my_id << "] Test finished. Server is running. Press Ctrl+C to exit." << std::endl;
        while(true) {
            sleep(10);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}