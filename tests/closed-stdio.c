#include <unistd.h>
int main(int argc, char **argv)
{
	if (argc < 2)
		return 1;
	close(0);
	close(1);
	close(2);
	execv(argv[1], argv + 1);
	return 123;
}
