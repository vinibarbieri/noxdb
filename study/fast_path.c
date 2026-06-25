#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>   // Define as flags O_WRONLY, O_CREAT, etc.
#include <unistd.h>  // Define as syscalls write() e close()
#include <string.h>  // Define a função memset()

#define FILE_SIZE (10 * 1024 * 1024) // 10 MB em bytes
#define FILE_NAME "test_data.bin"

int main() {
    printf("Iniciando alocacao de 10MB com malloc...\n");
    
    // 1. Alocar memória na Heap usando malloc padrão
    char *buffer = (char *)malloc(FILE_SIZE); // ponteiro buffer aloca file_size bits do tipo char
    if (buffer == NULL) {
        perror("Erro ao alocar memoria");
        return 1;
    }

    // Preencher o buffer com um dado fictício (a letra 'A')
    memset(buffer, 'A', FILE_SIZE);

    // 2. Abrir o arquivo (Syscall POSIX)
    // O_WRONLY: Apenas escrita
    // O_CREAT: Cria o arquivo se não existir
    // O_TRUNC: Zera o arquivo se ele já existir
    // 0644: Permissões de arquivo (rw-r--r--)
    int fd = open(FILE_NAME, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("Erro ao abrir o arquivo");
        free(buffer);
        return 1;
    }

    // 3. Escrever os dados no disco (Syscall POSIX)
    ssize_t bytes_written = write(fd, buffer, FILE_SIZE);
    if (bytes_written == -1) {
        perror("Erro ao escrever no arquivo");
        close(fd);
        free(buffer);
        return 1;
    }

    printf("Sucesso! %zd bytes escritos no arquivo '%s'.\n", bytes_written, FILE_NAME);

    // 4. Limpar a casa
    close(fd);
    free(buffer);

    return 0;
}