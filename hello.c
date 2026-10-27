#include <stdio.h>
#include <stdbool.h>

int main(){
    char a;
    printf("Hello, World!");
    printf("Do you want TEE? ");
    scanf("%c", &a);
    if(a == 'Y'){
        printf("TEE is not implemented");
        return 1;
    }else{
        printf("Bye!");
        return 0;
    }
}
