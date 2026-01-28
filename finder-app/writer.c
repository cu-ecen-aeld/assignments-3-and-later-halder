#include <stdio.h>
#include <syslog.h>

int main(int argc, char *argv[])
{
	openlog(argv[0], LOG_PID, LOG_USER);
	FILE *writefile = fopen(argv[1], "w");


	if (argc != 3)
	{
		syslog(LOG_ERR, "%s", "Error: Not enough arguments provided. Exiting program.");
		printf("Must specify full path to file and content to be written!\n");
		return 1;
	} else {
		if (writefile == NULL)
		{
			syslog(LOG_ERR, "Error: Could not open file %s. Exiting program", argv[1]);
			printf("Error: Could not open file %s. Exiting program", argv[1]);
			return 1;
		}

		syslog(LOG_DEBUG, "Writing %s to %s", argv[2], argv[1]);
		fprintf(writefile, "%s", argv[2]);
		
		fclose(writefile);
		return 0;
	}
}
