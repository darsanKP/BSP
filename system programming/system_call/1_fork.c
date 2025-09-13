#include<stdio.h>

#include<unistd.h>
#include<stdlib.h>
int main()
{
    int p =fork();
    if(p<0)
    {
        printf("error\n");
        exit(0);
    }
    else if(p==0)
    {
printf(" child process pid -%\n");
char *argv[] = {"/bin/pwd", NULL, NULL};
char *envp[] = {NULL};
execve("/bin/pwd", argv, envp);
    }
    else
    {
        printf("parent process \n");
    }

    return 0;
}