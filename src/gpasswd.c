/*
 * Copyright (c) 1990 - 1994, Julianne Frances Haugh
 * Copyright (c) 1996 - 2000, Marek Michałkiewicz
 * Copyright (c) 2001 - 2006, Tomasz Kłoczko
 * Copyright (c) 2007 - 2008, Nicolas François
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the copyright holders or contributors may not be used to
 *    endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#ident "$Id$"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include "defines.h"
#include "exitcodes.h"
#include "groupio.h"
#include "nscd.h"
#include "prototypes.h"
#ifdef SHADOWGRP
#include "sgroupio.h"
#endif
/*
 * Global variables
 */
/* The name of this command, as it is invoked */
static char *Prog;

#ifdef SHADOWGRP
/* Indicate if shadow groups are enabled on the system
 * (/etc/gshadow present) */
static bool is_shadowgrp;
static bool gshadow_locked = false;
#endif
static bool group_locked = false;

/* Flags set by options */
static bool aflg = false;
static bool Aflg = false;
static bool dflg = false;
static bool Mflg = false;
static bool rflg = false;
static bool Rflg = false;
/* The name of the group that is being affected */
static char *group = NULL;
/* The name of the user being added (-a) or removed (-d) from group */
static char *user = NULL;
/* The new list of members set with -M */
static char *members = NULL;
#ifdef SHADOWGRP
/* The new list of group administrators set with -A */
static char *admins = NULL;
#endif
/* The name of the caller */
static char *myname = NULL;
/* The UID of the caller */
static uid_t bywho;
/* Indicate if gpasswd was called by root */
#define amroot	(0 == bywho)

/* The number of retries for th user to provide and repeat a new password */
#ifndef	RETRIES
#define	RETRIES	3
#endif

/* local function prototypes */
static void usage (void);
static RETSIGTYPE catch_signals (int killed);
static void fail_exit (int status);
static bool is_valid_user_list (const char *users);
static void process_flags (int argc, char **argv);
static void check_flags (int argc, int opt_index);
static void open_files (void);
static void close_files (void);
#ifdef SHADOWGRP
static void get_group (struct group *gr, struct sgrp *sg);
static void check_perms (const struct group *gr, const struct sgrp *sg);
static void update_group (struct group *gr, struct sgrp *sg);
static void change_passwd (struct group *gr, struct sgrp *sg);
#else
static void get_group (struct group *gr);
static void check_perms (const struct group *gr);
static void update_group (struct group *gr);
static void change_passwd (struct group *gr);
#endif

/*
 * usage - display usage message
 */
static void usage (void)
{
	fprintf (stderr, _("Usage: %s [-r|-R] group\n"), Prog);
	fprintf (stderr, _("       %s [-a user] group\n"), Prog);
	fprintf (stderr, _("       %s [-d user] group\n"), Prog);
#ifdef SHADOWGRP
	fprintf (stderr,
		 _("       %s [-A user,...] [-M user,...] group\n"), Prog);
#else
	fprintf (stderr, _("       %s [-M user,...] group\n"), Prog);
#endif
	exit (E_USAGE);
}

/*
 * catch_signals - set or reset termio modes.
 *
 *	catch_signals() is called before processing begins. signal() is then
 *	called with catch_signals() as the signal handler. If signal later
 *	calls catch_signals() with a signal number, the terminal modes are
 *	then reset.
 */
static RETSIGTYPE catch_signals (int killed)
{
	static TERMIO sgtty;

	if (0 != killed) {
		STTY (0, &sgtty);
	} else {
		GTTY (0, &sgtty);
	}

	if (0 != killed) {
		(void) putchar ('\n');
		(void) fflush (stdout);
		fail_exit (killed);
	}
}

/*
 * fail_exit - undo as much as possible
 */
static void fail_exit (int status)
{
	if (group_locked) {
		if (gr_unlock () == 0) {
			fprintf (stderr, _("%s: cannot unlock the group file\n"), Prog);
			SYSLOG ((LOG_WARN, "cannot unlock the group file"));
#ifdef WITH_AUDIT
			audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
			              "unlocking group file",
			              group, AUDIT_NO_ID, 0);
#endif
		}
	}
#ifdef SHADOWGRP
	if (gshadow_locked) {
		if (sgr_unlock () == 0) {
			fprintf (stderr, _("%s: cannot unlock the shadow group file\n"), Prog);
			SYSLOG ((LOG_WARN, "cannot unlock the shadow group file"));
#ifdef WITH_AUDIT
			audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
			              "unlocking gshadow file",
			              group, AUDIT_NO_ID, 0);
#endif
		}
	}
#endif

	exit (status);
}

/*
 * is_valid_user_list - check a comma-separated list of user names for validity
 *
 *	is_valid_user_list scans a comma-separated list of user names and
 *	checks that each listed name exists is the user database.
 *
 *	It returns true if the list of users is valid.
 */
static bool is_valid_user_list (const char *users)
{
	const char *start, *end;
	char username[32];
	bool valid = true;
	size_t len;

	for (start = users; (NULL != start) && ('\0' != *start); start = end) {
		end = strchr (start, ',');
		if (NULL != end) {
			len = (size_t) (end - start);
			end++;
		} else {
			len = strlen (start);
		}

		if (len > sizeof (username) - 1) {
			len = sizeof (username) - 1;
		}
		strncpy (username, start, len);
		username[len] = '\0';

		/*
		 * This user must exist.
		 */

		/* local, no need for xgetpwnam */
		if (getpwnam (username) == NULL) {
			fprintf (stderr, _("%s: user '%s' does not exist\n"),
			         Prog, username);
			valid = false;
		}
	}
	return valid;
}

static void failure (void)
{
	fprintf (stderr, _("%s: Permission denied.\n"), Prog);
	fail_exit (1);
}

/*
 * process_flags - process the command line options and arguments
 */
static void process_flags (int argc, char **argv)
{
	int flag;

	while ((flag = getopt (argc, argv, "a:A:d:gM:rR")) != EOF) {
		switch (flag) {
		case 'a':	/* add a user */
			user = optarg;
			/* local, no need for xgetpwnam */
			if (getpwnam (user) == NULL) {
				fprintf (stderr,
				         _("%s: user '%s' does not exist\n"), Prog,
				         user);
#ifdef WITH_AUDIT
				audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
				              "adding to group",
				              user, AUDIT_NO_ID, 0);
#endif
				fail_exit (1);
			}
			aflg = true;
			break;
#ifdef SHADOWGRP
		case 'A':
			if (!amroot) {
#ifdef WITH_AUDIT
				audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
				              "Listing administrators",
				              NULL, (unsigned int) bywho, 0);
#endif
				failure ();
			}
			if (!is_shadowgrp) {
				fprintf (stderr,
					 _("%s: shadow group passwords required for -A\n"),
					 Prog);
				fail_exit (2);
			}
			admins = optarg;
			if (!is_valid_user_list (admins)) {
				fail_exit (1);
			}
			Aflg = true;
			break;
#endif
		case 'd':	/* delete a user */
			dflg = true;
			user = optarg;
			break;
		case 'g':	/* no-op from normal password */
			break;
		case 'M':
			if (!amroot) {
#ifdef WITH_AUDIT
				audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
				              "listing members",
				              NULL, (unsigned int) bywho, 0);
#endif
				failure ();
			}
			members = optarg;
			if (!is_valid_user_list (members)) {
				fail_exit (1);
			}
			Mflg = true;
			break;
		case 'r':	/* remove group password */
			rflg = true;
			break;
		case 'R':	/* restrict group password */
			Rflg = true;
			break;
		default:
			usage ();
		}
	}

	/* Get the name of the group that is being affected. */
	group = argv[optind];

	check_flags (argc, optind);
}

/*
 * check_flags - check the validity of options
 */
static void check_flags (int argc, int opt_index)
{
	int exclusive = 0;
	/*
	 * Make sure exclusive flags are exclusive
	 */
	if (aflg) {
		exclusive++;
	}
	if (dflg) {
		exclusive++;
	}
	if (rflg) {
		exclusive++;
	}
	if (Rflg) {
		exclusive++;
	}
	if (Aflg || Mflg) {
		exclusive++;
	}
	if (exclusive > 1) {
		usage ();
	}

	/*
	 * Make sure one (and only one) group was provided
	 */
	if ((argc != (opt_index+1)) || (NULL == group)) {
		usage ();
	}
}

/*
 * open_files - lock and open the group databases
 *
 *	It will call exit in case of error.
 */
static void open_files (void)
{
	if (gr_lock () == 0) {
		fprintf (stderr, _("%s: cannot lock the group file\n"), Prog);
		SYSLOG ((LOG_WARN, "cannot lock the group file"));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "locking /etc/group",
		              group, AUDIT_NO_ID, 0);
#endif
		fail_exit (1);
	}
	group_locked = true;
#ifdef SHADOWGRP
	if (is_shadowgrp) {
		if (sgr_lock () == 0) {
			fprintf (stderr,
			         _("%s: cannot lock the shadow group file\n"), Prog);
			SYSLOG ((LOG_WARN, "cannot lock the shadow group file"));
#ifdef WITH_AUDIT
			audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
			              "locking /etc/gshadow",
			              group, AUDIT_NO_ID, 0);
#endif
			fail_exit (1);
		}
		gshadow_locked = true;
	}
#endif
	if (gr_open (O_RDWR) == 0) {
		fprintf (stderr, _("%s: cannot open the group file\n"), Prog);
		SYSLOG ((LOG_WARN, "cannot open the group file"));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "opening /etc/group",
		              group, AUDIT_NO_ID, 0);
#endif
		fail_exit (1);
	}
#ifdef SHADOWGRP
	if (is_shadowgrp && (sgr_open (O_RDWR) == 0)) {
		fprintf (stderr, _("%s: cannot open the shadow group file\n"), Prog);
		SYSLOG ((LOG_WARN, "cannot open the shadow group file"));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "opening /etc/gshadow",
		              group, AUDIT_NO_ID, 0);
#endif
		fail_exit (1);
	}
#endif
}

/*
 * close_files - close and unlock the group databases
 *
 *	This cause any changes in the databases to be committed.
 *
 *	It will call exit in case of error.
 */
static void close_files (void)
{
	if (gr_close () == 0) {
		fprintf (stderr, _("%s: cannot rewrite the group file\n"), Prog);
		SYSLOG ((LOG_WARN, "cannot rewrite the group file"));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "rewriting /etc/group",
		              group, AUDIT_NO_ID, 0);
#endif
		fail_exit (1);
	}
#ifdef SHADOWGRP
	if (is_shadowgrp && (sgr_close () == 0)) {
		fprintf (stderr, _("%s: cannot rewrite the shadow group file\n"), Prog);
		SYSLOG ((LOG_WARN, "cannot rewrite the shadow group file"));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "rewriting /etc/gshadow",
		              group, AUDIT_NO_ID, 0);
#endif
		fail_exit (1);
	}
	if (is_shadowgrp) {
		if (sgr_unlock () == 0) {
			fprintf (stderr, _("%s: cannot unlock the shadow group file\n"), Prog);
			SYSLOG ((LOG_WARN, "cannot unlock the shadow group file"));
#ifdef WITH_AUDIT
			audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
			              "unlocking gshadow file",
			              group, AUDIT_NO_ID, 0);
#endif
		}
		gshadow_locked = false;
	}
#endif
	if (gr_unlock () == 0) {
		fprintf (stderr, _("%s: cannot unlock the group file\n"), Prog);
		SYSLOG ((LOG_WARN, "cannot unlock the group file"));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "unlocking group file",
		              group, AUDIT_NO_ID, 0);
#endif
	}
	group_locked = false;
}

/*
 * check_perms - check if the user is allowed to change the password of
 *               the specified group.
 *
 *	It only returns if the user is allowed.
 */
#ifdef SHADOWGRP
static void check_perms (const struct group *gr, const struct sgrp *sg)
#else
static void check_perms (const struct group *gr)
#endif
{
#ifdef SHADOWGRP
	if (is_shadowgrp) {
		/*
		 * The policy here for changing a group is that
		 * 1) you must be root or
		 * 2) you must be listed as an administrative member.
		 * Administrative members can do anything to a group that
		 * the root user can.
		 */
		if (!amroot && !is_on_list (sg->sg_adm, myname)) {
#ifdef WITH_AUDIT
			audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
			              "modify group",
			              group, AUDIT_NO_ID, 0);
#endif
			failure ();
		}
	} else
#endif				/* ! SHADOWGRP */
	{
#ifdef FIRST_MEMBER_IS_ADMIN
		/*
		 * The policy here for changing a group is that
		 * 1) you must be root or
		 * 2) you must be the first listed member of the group.
		 * The first listed member of a group can do anything to
		 * that group that the root user can. The rationale for
		 * this hack is that the FIRST user is probably the most
		 * important user in this entire group.
		 *
		 * This feature enabled by default could be a security
		 * problem when installed on existing systems where the
		 * first group member might be just a normal user.
		 * --marekm
		 */
		if (!amroot) {
			if (gr->gr_mem[0] == (char *) 0) {
#ifdef WITH_AUDIT
				audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
				              "modifying group",
				              group, AUDIT_NO_ID, 0);
#endif
				failure ();
			}

			if (strcmp (gr->gr_mem[0], myname) != 0) {
#ifdef WITH_AUDIT
				audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
				              "modifying group",
				              myname, AUDIT_NO_ID, 0);
#endif
				failure ();
			}
		}
#else				/* ! FIRST_MEMBER_IS_ADMIN */
		if (!amroot) {
#ifdef WITH_AUDIT
			audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
			              "modifying group",
			              group, AUDIT_NO_ID, 0);
#endif
			failure ();
		}
#endif
	}
}

/*
 * update_group - Update the group information in the databases
 */
#ifdef SHADOWGRP
static void update_group (struct group *gr, struct sgrp *sg)
#else
static void update_group (struct group *gr)
#endif
{
	if (gr_update (gr) == 0) {
		fprintf (stderr, _("%s: cannot update the entry of '%s' in the group file\n"), Prog, gr->gr_name);
		SYSLOG ((LOG_WARN, "cannot update the entry of '%s' in the group file", gr->gr_name));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "updating /etc/group",
		              group, AUDIT_NO_ID, 0);
#endif
		fail_exit (1);
	}
#ifdef SHADOWGRP
	if (is_shadowgrp && (sgr_update (sg) == 0)) {
		fprintf (stderr, _("%s: cannot update the entry of '%s' in the shadow group file\n"), Prog, sg->sg_name);
		SYSLOG ((LOG_WARN, "cannot update the entry of '%s' in the shadow group file", sg->sg_name));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "updating /etc/gshadow",
		              group, AUDIT_NO_ID, 0);
#endif
		fail_exit (1);
	}
#endif
}

/*
 * get_group - get the current information for the group
 *
 *	The information are copied in group structure(s) so that they can be
 *	modified later.
 *
 *	Note: If !is_shadowgrp, *sg will not be initialized.
 */
#ifdef SHADOWGRP
static void get_group (struct group *gr, struct sgrp *sg)
#else
static void get_group (struct group *gr)
#endif
{
	struct group const*tmpgr = NULL;
	struct sgrp const*tmpsg = NULL;

	if (gr_open (O_RDONLY) == 0) {
		fprintf (stderr, _("%s: cannot open the group file\n"), Prog);
		SYSLOG ((LOG_WARN, "cannot open the group file"));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "opening /etc/group",
		              group, AUDIT_NO_ID, 0);
#endif
		fail_exit (1);
	}

	tmpgr = gr_locate (group);
	if (NULL == tmpgr) {
		fprintf (stderr, _("%s: group '%s' does not exist in the group file\n"), Prog, group);
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "group lookup",
		              group, AUDIT_NO_ID, 0);
#endif
		failure ();
	}

	*gr = *tmpgr;
	gr->gr_name = xstrdup (tmpgr->gr_name);
	gr->gr_passwd = xstrdup (tmpgr->gr_passwd);
	gr->gr_mem = dup_list (tmpgr->gr_mem);

	if (gr_close () == 0) {
		fprintf (stderr, _("%s: cannot rewrite the group file\n"), Prog);
		SYSLOG ((LOG_WARN, "cannot rewrite the group file"));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "closing /etc/group",
		              group, AUDIT_NO_ID, 0);
#endif
		fail_exit (1);
	}

#ifdef SHADOWGRP
	if (is_shadowgrp) {
		if (sgr_open (O_RDONLY) == 0) {
			fprintf (stderr,
			         _("%s: cannot open the shadow group file\n"), Prog);
			SYSLOG ((LOG_WARN, "cannot open the shadow group file"));
#ifdef WITH_AUDIT
			audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
			              "opening /etc/gshadow",
			              group, AUDIT_NO_ID, 0);
#endif
			fail_exit (1);
		}
		tmpsg = sgr_locate (group);
		if (NULL != tmpsg) {
			*sg = *tmpsg;
			sg->sg_name = xstrdup (tmpsg->sg_name);
			sg->sg_passwd = xstrdup (tmpsg->sg_passwd);

			sg->sg_mem = dup_list (tmpsg->sg_mem);
			sg->sg_adm = dup_list (tmpsg->sg_adm);
		} else {
			sg->sg_name = xstrdup (group);
			sg->sg_passwd = gr->gr_passwd;
			gr->gr_passwd = SHADOW_PASSWD_STRING;	/* XXX warning: const */

			sg->sg_mem = dup_list (gr->gr_mem);

			sg->sg_adm = (char **) xmalloc (sizeof (char *) * 2);
#ifdef FIRST_MEMBER_IS_ADMIN
			if (sg->sg_mem[0]) {
				sg->sg_adm[0] = xstrdup (sg->sg_mem[0]);
				sg->sg_adm[1] = NULL;
			} else
#endif
			{
				sg->sg_adm[0] = NULL;
			}

		}
		if (sgr_close () == 0) {
			fprintf (stderr,
			         _("%s: cannot rewrite the shadow group file\n"), Prog);
			SYSLOG ((LOG_WARN, "cannot rewrite the shadow group file"));
#ifdef WITH_AUDIT
			audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
			              "closing /etc/gshadow",
			              group, AUDIT_NO_ID, 0);
#endif
			fail_exit (1);
		}
	}
#endif				/* SHADOWGRP */
}

/*
 * change_passwd - change the group's password
 *
 *	Get the new password from the user and update the password in the
 *	group's structure.
 *
 *	It will call exit in case of error.
 */
#ifdef SHADOWGRP
static void change_passwd (struct group *gr, struct sgrp *sg)
#else
static void change_passwd (struct group *gr)
#endif
{
	char *cp;
	static char pass[BUFSIZ];
	int retries;

	/*
	 * A new password is to be entered and it must be encrypted, etc.
	 * The password will be prompted for twice, and both entries must be
	 * identical. There is no need to validate the old password since
	 * the invoker is either the group owner, or root.
	 */
	printf (_("Changing the password for group %s\n"), group);

	for (retries = 0; retries < RETRIES; retries++) {
		cp = getpass (_("New Password: "));
		if (NULL == cp) {
			fail_exit (1);
		}

		STRFCPY (pass, cp);
		strzero (cp);
		cp = getpass (_("Re-enter new password: "));
		if (NULL == cp) {
			fail_exit (1);
		}

		if (strcmp (pass, cp) == 0) {
			strzero (cp);
			break;
		}

		strzero (cp);
		memzero (pass, sizeof pass);

		if (retries + 1 < RETRIES) {
			puts (_("They don't match; try again"));
#ifdef WITH_AUDIT
			audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
			              "changing password",
			              group, AUDIT_NO_ID, 0);
#endif
		}
	}

	if (retries == RETRIES) {
		fprintf (stderr, _("%s: Try again later\n"), Prog);
		fail_exit (1);
	}

	cp = pw_encrypt (pass, crypt_make_salt (NULL, NULL));
	memzero (pass, sizeof pass);
#ifdef SHADOWGRP
	if (is_shadowgrp) {
		sg->sg_passwd = cp;
	} else
#endif
	{
		gr->gr_passwd = cp;
	}
#ifdef WITH_AUDIT
	audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
	              "changing password",
	              group, AUDIT_NO_ID, 1);
#endif
	SYSLOG ((LOG_INFO, "change the password for group %s by %s", group,
	        myname));
}

/*
 * gpasswd - administer the /etc/group file
 *
 *	-a user		add user to the named group
 *	-d user		remove user from the named group
 *	-r		remove password from the named group
 *	-R		restrict access to the named group
 *	-A user,...	make list of users the administrative users
 *	-M user,...	make list of users the group members
 */
int main (int argc, char **argv)
{
	struct group grent;
#ifdef SHADOWGRP
	struct sgrp sgent;
#endif
	struct passwd *pw = NULL;

#ifdef WITH_AUDIT
	audit_help_open ();
#endif

	sanitize_env ();
	(void) setlocale (LC_ALL, "");
	(void) bindtextdomain (PACKAGE, LOCALEDIR);
	(void) textdomain (PACKAGE);

	/*
	 * Make a note of whether or not this command was invoked by root.
	 * This will be used to bypass certain checks later on. Also, set
	 * the real user ID to match the effective user ID. This will
	 * prevent the invoker from issuing signals which would interfer
	 * with this command.
	 */
	bywho = getuid ();
	Prog = Basename (argv[0]);

	OPENLOG ("gpasswd");
	setbuf (stdout, NULL);
	setbuf (stderr, NULL);

#ifdef SHADOWGRP
	is_shadowgrp = sgr_file_present ();
#endif

	/* Parse the options */
	process_flags (argc, argv);

	/*
	 * Determine the name of the user that invoked this command. This
	 * is really hit or miss because there are so many ways that command
	 * can be executed and so many ways to trip up the routines that
	 * report the user name.
	 */

	pw = get_my_pwent ();
	if (NULL == pw) {
		fputs (_("Who are you?\n"), stderr);
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "user lookup",
		              NULL, (unsigned int) bywho, 0);
#endif
		failure ();
	}
	myname = xstrdup (pw->pw_name);

	/*
	 * Replicate the group so it can be modified later on.
	 */
#ifdef SHADOWGRP
	get_group (&grent, &sgent);
#else
	get_group (&grent);
#endif

	/*
	 * Check if the user is allowed to change the password of this group.
	 */
#ifdef SHADOWGRP
	check_perms (&grent, &sgent);
#else
	check_perms (&grent);
#endif

	/*
	 * Removing a password is straight forward. Just set the password
	 * field to a "".
	 */
	if (rflg) {
		grent.gr_passwd = "";	/* XXX warning: const */
#ifdef SHADOWGRP
		sgent.sg_passwd = "";	/* XXX warning: const */
#endif
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "deleting group password",
		              group, AUDIT_NO_ID, 1);
#endif
		SYSLOG ((LOG_INFO, "remove password from group %s by %s",
		         group, myname));
		goto output;
	} else if (Rflg) {
		/*
		 * Same thing for restricting the group. Set the password
		 * field to "!".
		 */
		grent.gr_passwd = "!";	/* XXX warning: const */
#ifdef SHADOWGRP
		sgent.sg_passwd = "!";	/* XXX warning: const */
#endif
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "restrict access to group",
		              group, AUDIT_NO_ID, 1);
#endif
		SYSLOG ((LOG_INFO, "restrict access to group %s by %s",
			 group, myname));
		goto output;
	}

	/*
	 * Adding a member to a member list is pretty straightforward as
	 * well. Call the appropriate routine and split.
	 */
	if (aflg) {
		printf (_("Adding user %s to group %s\n"), user, group);
		grent.gr_mem = add_list (grent.gr_mem, user);
#ifdef SHADOWGRP
		if (is_shadowgrp) {
			sgent.sg_mem = add_list (sgent.sg_mem, user);
		}
#endif
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "adding group member",
		              user, AUDIT_NO_ID, 1);
#endif
		SYSLOG ((LOG_INFO, "add member %s to group %s by %s", user,
		         group, myname));
		goto output;
	}

	/*
	 * Removing a member from the member list is the same deal as adding
	 * one, except the routine is different.
	 */
	if (dflg) {
		bool removed = false;

		printf (_("Removing user %s from group %s\n"), user, group);

		if (is_on_list (grent.gr_mem, user)) {
			removed = true;
			grent.gr_mem = del_list (grent.gr_mem, user);
		}
#ifdef SHADOWGRP
		if (is_shadowgrp) {
			if (is_on_list (sgent.sg_mem, user)) {
				removed = true;
				sgent.sg_mem = del_list (sgent.sg_mem, user);
			}
		}
#endif
		if (!removed) {
			fprintf (stderr, _("%s: user '%s' is not a member of '%s'\n"),
			         Prog, user, group);
#ifdef WITH_AUDIT
			audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
			              "deleting member",
			              user, AUDIT_NO_ID, 0);
#endif
			fail_exit (1);
		}
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "deleting member",
		              user, AUDIT_NO_ID, 1);
#endif
		SYSLOG ((LOG_INFO, "remove member %s from group %s by %s",
		         user, group, myname));
		goto output;
	}
#ifdef SHADOWGRP
	/*
	 * Replacing the entire list of administrators is simple. Check the
	 * list to make sure everyone is a real user. Then slap the new list
	 * in place.
	 */
	if (Aflg) {
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "setting group admin",
		              group, AUDIT_NO_ID, 1);
#endif
		SYSLOG ((LOG_INFO, "set administrators of %s to %s",
		         group, admins));
		sgent.sg_adm = comma_to_list (admins);
		if (!Mflg) {
			goto output;
		}
	}
#endif

	/*
	 * Replacing the entire list of members is simple. Check the list to
	 * make sure everyone is a real user. Then slap the new list in
	 * place.
	 */
	if (Mflg) {
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "setting group members",
		              group, AUDIT_NO_ID, 1);
#endif
		SYSLOG ((LOG_INFO, "set members of %s to %s", group, members));
#ifdef SHADOWGRP
		sgent.sg_mem = comma_to_list (members);
#endif
		grent.gr_mem = comma_to_list (members);
		goto output;
	}

	/*
	 * If the password is being changed, the input and output must both
	 * be a tty. The typical keyboard signals are caught so the termio
	 * modes can be restored.
	 */
	if ((isatty (0) == 0) || (isatty (1) == 0)) {
		fprintf (stderr, _("%s: Not a tty\n"), Prog);
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "changing password",
		              group, AUDIT_NO_ID, 0);
#endif
		fail_exit (1);
	}

	catch_signals (0);	/* save tty modes */

	(void) signal (SIGHUP, catch_signals);
	(void) signal (SIGINT, catch_signals);
	(void) signal (SIGQUIT, catch_signals);
	(void) signal (SIGTERM, catch_signals);
#ifdef SIGTSTP
	(void) signal (SIGTSTP, catch_signals);
#endif

	/* Prompt for the new password */
#ifdef SHADOWGRP
	change_passwd (&grent, &sgent);
#else
	change_passwd (&grent);
#endif

	/*
	 * This is the common arrival point to output the new group file.
	 * The freshly crafted entry is in allocated space. The group file
	 * will be locked and opened for writing. The new entry will be
	 * output, etc.
	 */
      output:
	if (setuid (0) != 0) {
		fputs (_("Cannot change ID to root.\n"), stderr);
		SYSLOG ((LOG_ERR, "can't setuid(0)"));
#ifdef WITH_AUDIT
		audit_logger (AUDIT_USER_CHAUTHTOK, Prog,
		              "changing id to root",
		              group, AUDIT_NO_ID, 0);
#endif
		closelog ();
		fail_exit (1);
	}
	pwd_init ();

	open_files ();

#ifdef SHADOWGRP
	update_group (&grent, &sgent);
#else
	update_group (&grent);
#endif

	close_files ();

	nscd_flush_cache ("group");

	exit (E_SUCCESS);
}

