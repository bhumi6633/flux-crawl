#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>

int main() {
    // The input data
    const char *input = "Hello, world!";
    unsigned char hash[SHA256_DIGEST_LENGTH];

    // Compute SHA-256 hash
    SHA256((const unsigned char *)input, strlen(input), hash);

    // Print the hash as hexadecimal
    printf("SHA-256 hash: ");
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        printf("%02x", hash[i]);
    printf("\n");

    return 0;
}