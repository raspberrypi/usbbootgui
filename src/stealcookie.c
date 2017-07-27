/**
 * Code to borrow X authorization session cookie of currently logged in user
 * so we can display the GUI on screen despite it being started by udev
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <utmp.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <pwd.h>

/* Parse /var/run/utmp looking for the user logged into X */
static char *getLoggedInUser()
{
	char *username = NULL;
	struct utmp entry;
	FILE *file = fopen("/var/run/utmp", "rb");

	if (file)
	{
		while ( fread(&entry, sizeof(entry), 1, file) == 1 )
		{
			/* We are interested in the owner of a user process started on :0 */
			if (entry.ut_type == USER_PROCESS && strcmp(entry.ut_host, ":0") == 0)
			{
				/* Using strndup as entries are not null-terminated if they are maximum size */
				username = strndup(entry.ut_user, sizeof(entry.ut_user));
				break;
			}
		}

		fclose(file);
	}

	return username;
}

/* Borrow X authorization cookie of logged-in user
 * Sets XAUTHORITY and DISPLAY environement variables */
void stealcookie()
{
	char xauthority[PATH_MAX];
	struct passwd *pwdentry;
	char *username = getLoggedInUser();

	while (!username)
	{
		/* No user is logged in yet. Try again in a second */
		sleep(1);
		username = getLoggedInUser();
		if (username)
		{
			/* User has just logged in. Delay a bit more to let window manager start first */
			sleep(4); 
		}
	}

	pwdentry = getpwnam(username);
	if (pwdentry && pwdentry->pw_dir && strlen(pwdentry->pw_dir))
	{
		/* Test if a system wide authority dir is in use first */
		snprintf(xauthority, sizeof(xauthority), "/var/run/lightdm/%s/xauthority", username);
		
		if (access(xauthority, F_OK) != 0)
		{
			/* Borrow .Xauthority file in user's home directory instead */
			snprintf(xauthority, sizeof(xauthority), "%s/.Xauthority", pwdentry->pw_dir);
		}
		setenv("XAUTHORITY", xauthority, 1);
		setenv("DISPLAY", ":0", 1);
	}

	free(username);
}
