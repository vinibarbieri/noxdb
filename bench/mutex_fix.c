#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

// Declare pthread_mutex_t -> PTHREAD_MUTEX_INITIALIZER
static pthread_mutex_t counterMutex = PTHREAD_MUTEX_INITIALIZER;

int counter = 0;

void* increment(void *arg) {
    // lock
    for (int i = 0; i < 1000000; i++) {
        pthread_mutex_lock(&counterMutex);
        counter++;
        pthread_mutex_unlock(&counterMutex);

    }
    //unlock
    return NULL;
}

int main() {
    pthread_t thread_a;
    pthread_t thread_b;
    int a;
    int b;

    a = pthread_create(&thread_a, NULL, increment, NULL);
    if (a != 0) {
        printf("pthread_create error: %d\n", a);
    }

    b = pthread_create(&thread_b, NULL, increment, NULL);
    if (b != 0) {
        printf("pthread_create error: %d\n", b);
    }

    a = pthread_join(thread_a, NULL);
    if (a != 0) {
        printf("pthread_join error: %d\n", a);
    }

    b = pthread_join(thread_b, NULL);
    if (b != 0) {
        printf("pthread_join error: %d\n", b);
    }

    printf("Counter: %d\n", counter);

    return 0;
}