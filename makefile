# Compilador C++
CXX = g++

# Flags de compilação
# -std=c++11: para usar threads e outras funcionalidades modernas
# -g: para incluir informações de debug
# -Wall: para mostrar todos os warnings
# -lpthread: para linkar a biblioteca de pthreads
CXXFLAGS = -std=c++11 -g -Wall
LDFLAGS = -lpthread

# Nome do executável final
TARGET = dsm_app

# Lista de arquivos-fonte (.cpp)
SOURCES = main.cpp dsm.cpp server.cpp

# Gera a lista de arquivos-objeto (.o) a partir dos fontes
OBJECTS = $(SOURCES:.cpp=.o)

# Regra principal: compila o programa inteiro
all: $(TARGET)

# Regra para linkar os arquivos-objeto e criar o executável
$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS) $(LDFLAGS)

# Regra para compilar cada arquivo-fonte .cpp em um arquivo-objeto .o
%.o: %.cpp dsm.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Regra para limpar os arquivos gerados pela compilação
clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean