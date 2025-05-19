#include <string.h>
#include <stdlib.h>

int secure_function() {
	exit(0);
	return 1;
}

void copy_name(char *who){
	char name[10];
	strcpy(name, who);
}

int main(){
	copy_name("Quentin");
	copy_name("AAAAAAAAAAAAAAAAAAAAAAAA\x58\x08\xaa\xaa\xaa\xaa\0");
	return 1;
}
