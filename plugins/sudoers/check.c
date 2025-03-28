/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 1993-1996,1998-2005, 2007-2018
 *	Todd C. Miller <Todd.Miller@sudo.ws>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

/*
 * This is an open source non-commercial project. Dear PVS-Studio, please check it.
 * PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
 */

#include <config.h>

#include <sys/types.h>			/* for ssize_t */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

#include "sudoers.h"
#include "check.h"

struct getpass_closure {
    int tstat;
    int lectured;
    void *cookie;
    struct passwd *auth_pw;
};

static struct passwd *get_authpw(int);

/*
 * Called when getpass is suspended so we can drop the lock.
 */
static int
getpass_suspend(int signo, void *vclosure)
{
    struct getpass_closure *closure = vclosure;

    timestamp_close(closure->cookie);
    closure->cookie = NULL;
    return 0;
}

/*
 * Called when getpass is resumed so we can reacquire the lock.
 */
static int
getpass_resume(int signo, void *vclosure)
{
    struct getpass_closure *closure = vclosure;

    closure->cookie = timestamp_open(user_name, user_sid);
    if (closure->cookie == NULL)
	return -1;
    if (!timestamp_lock(closure->cookie, closure->auth_pw))
	return -1;
    return 0;
}

/*
 * Returns true if the user successfully authenticates, false if not
 * or -1 on fatal error.
 */
static int
check_user_interactive(int validated, int mode, struct getpass_closure *closure)
{
    struct sudo_conv_callback callback;
    int ret = -1;
    char *prompt;
    debug_decl(check_user_interactive, SUDOERS_DEBUG_AUTH);

    /* Construct callback for getpass function. */
    memset(&callback, 0, sizeof(callback));
    callback.version = SUDO_CONV_CALLBACK_VERSION;
    callback.closure = closure;
    callback.on_suspend = getpass_suspend;
    callback.on_resume = getpass_resume;

    /* Open, lock and read time stamp file if we are using it. */
    if (!ISSET(mode, MODE_IGNORE_TICKET)) {
	/* Open time stamp file and check its status. */
	closure->cookie = timestamp_open(user_name, user_sid);
	if (closure->cookie != NULL) {
	    if (timestamp_lock(closure->cookie, closure->auth_pw)) {
		closure->tstat = timestamp_status(closure->cookie,
		    closure->auth_pw);
	    }
	    callback.on_suspend = getpass_suspend;
	    callback.on_resume = getpass_resume;
	}
    }

    switch (closure->tstat) {
    case TS_FATAL:
	/* Fatal error (usually setuid failure), unsafe to proceed. */
	goto done;

    case TS_CURRENT:
	/* Time stamp file is valid and current. */
	if (!ISSET(validated, FLAG_CHECK_USER)) {
	    ret = true;
	    break;
	}
	sudo_debug_printf(SUDO_DEBUG_INFO,
	    "%s: check user flag overrides time stamp", __func__);
	FALLTHROUGH;

    default:
	if (ISSET(mode, MODE_NONINTERACTIVE) && !def_noninteractive_auth) {
	    validated |= FLAG_NO_USER_INPUT;
	    log_auth_failure(validated, 0);
	    goto done;
	}

	/* Expand any escapes in the prompt. */
	prompt = expand_prompt(user_prompt ? user_prompt : def_passprompt,
	    closure->auth_pw->pw_name);
	if (prompt == NULL)
	    goto done;

	ret = verify_user(closure->auth_pw, prompt, validated, &callback);
	if (ret == true && closure->lectured)
	    (void)set_lectured();	/* lecture error not fatal */
	free(prompt);
	break;
    }

done:
    debug_return_int(ret);
}

/*
 * Returns true if the user successfully authenticates, false if not
 * or -1 on error.
 */
int
check_user(int validated, int mode)
{
    struct getpass_closure closure = { TS_ERROR };
    int ret = -1;
    bool exempt = false;
    debug_decl(check_user, SUDOERS_DEBUG_AUTH);

    /*
     * In intercept mode, only check the user if configured to do so.
     * We already have a session so no need to init the auth subsystem.
     */
    if (ISSET(sudo_mode, MODE_POLICY_INTERCEPTED)) {
	if (!def_intercept_authenticate) {
	    debug_return_int(true);
	}
    }

    /*
     * Init authentication system regardless of whether we need a password.
     * Required for proper PAM session support.
     */
    if ((closure.auth_pw = get_authpw(mode)) == NULL)
	goto done;
    if (sudo_auth_init(closure.auth_pw, mode) == -1)
	goto done;

    /*
     * Don't prompt for the root passwd or if the user is exempt.
     * If the user is not changing uid/gid, no need for a password.
     */
    if (!def_authenticate || user_is_exempt()) {
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: %s", __func__,
	    !def_authenticate ? "authentication disabled" :
	    "user exempt from authentication");
	exempt = true;
	ret = true;
	goto done;
    }
    if (user_uid == 0 || (user_uid == runas_pw->pw_uid &&
	(!runas_gr || user_in_group(sudo_user.pw, runas_gr->gr_name)))) {
#ifdef HAVE_SELINUX
	if (user_role == NULL && user_type == NULL)
#endif
#ifdef HAVE_APPARMOR
	if (user_apparmor_profile == NULL)
#endif
#ifdef HAVE_PRIV_SET
	if (runas_privs == NULL && runas_limitprivs == NULL)
#endif
	{
	    sudo_debug_printf(SUDO_DEBUG_INFO,
		"%s: user running command as self", __func__);
	    ret = true;
	    goto done;
	}
    }

    ret = check_user_interactive(validated, mode, &closure);

done:
    if (ret == true) {
	/* The approval function may disallow a user post-authentication. */
	ret = sudo_auth_approval(closure.auth_pw, validated, exempt);

	/*
	 * Only update time stamp if user validated and was approved.
	 * Failure to update the time stamp is not a fatal error.
	 */
	if (ret == true && ISSET(validated, VALIDATE_SUCCESS)) {
	    if (ISSET(mode, MODE_UPDATE_TICKET) && closure.tstat != TS_ERROR)
		(void)timestamp_update(closure.cookie, closure.auth_pw);
	}
    }
    timestamp_close(closure.cookie);
    sudo_auth_cleanup(closure.auth_pw, !ISSET(validated, VALIDATE_SUCCESS));
    if (closure.auth_pw != NULL)
	sudo_pw_delref(closure.auth_pw);

    debug_return_int(ret);
}

/*
 * Display sudo lecture (standard or custom).
 * Returns true if the user was lectured, else false.
 */
void
display_lecture(struct sudo_conv_callback *callback)
{
    struct getpass_closure *closure;
    struct sudo_conv_message msg;
    struct sudo_conv_reply repl;
    char buf[BUFSIZ];
    struct stat sb;
    ssize_t nread;
    int fd;
    debug_decl(lecture, SUDOERS_DEBUG_AUTH);

    if (callback == NULL || (closure = callback->closure) == NULL)
	debug_return;

    if (closure->lectured)
	debug_return;

    if (def_lecture == never || (def_lecture == once && already_lectured()))
	debug_return;

    memset(&msg, 0, sizeof(msg));
    memset(&repl, 0, sizeof(repl));

    if (def_lecture_file) {
	fd = open(def_lecture_file, O_RDONLY|O_NONBLOCK);
	if (fd != -1 && fstat(fd, &sb) == 0) {
	    if (S_ISREG(sb.st_mode)) {
		(void) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
		while ((nread = read(fd, buf, sizeof(buf) - 1)) > 0) {
		    buf[nread] = '\0';
		    msg.msg_type = SUDO_CONV_ERROR_MSG|SUDO_CONV_PREFER_TTY;
		    msg.msg = buf;
		    sudo_conv(1, &msg, &repl, NULL);
		}
		if (nread == 0) {
		    close(fd);
		    goto done;
		}
		log_warning(SLOG_RAW_MSG,
		    N_("error reading lecture file %s"), def_lecture_file);
	    } else {
		log_warningx(SLOG_RAW_MSG,
		    N_("ignoring lecture file %s: not a regular file"),
		    def_lecture_file);
	    }
	} else {
	    log_warning(SLOG_RAW_MSG|SLOG_NO_STDERR, N_("unable to open %s"),
		def_lecture_file);
	}
	if (fd != -1)
	    close(fd);
    }

    /* Default sudo lecture. */
    msg.msg_type = SUDO_CONV_ERROR_MSG|SUDO_CONV_PREFER_TTY;
    msg.msg = _("\n"
	"We trust you have received the usual lecture from the local System\n"
	"Administrator. It usually boils down to these three things:\n\n"
	"    #1) Respect the privacy of others.\n"
	"    #2) Think before you type.\n"
	"    #3) With great power comes great responsibility.\n\n");
    sudo_conv(1, &msg, &repl, NULL);

done:
    closure->lectured = true;
    debug_return;
}

/*
 * Checks if the user is exempt from supplying a password.
 */
bool
user_is_exempt(void)
{
    bool ret = false;
    debug_decl(user_is_exempt, SUDOERS_DEBUG_AUTH);

    if (def_exempt_group) {
	if (user_in_group(sudo_user.pw, def_exempt_group))
	    ret = true;
    }
    debug_return_bool(ret);
}

/*
 * Get passwd entry for the user we are going to authenticate as.
 * By default, this is the user invoking sudo.  In the most common
 * case, this matches sudo_user.pw or runas_pw.
 */
static struct passwd *
get_authpw(int mode)
{
    struct passwd *pw = NULL;
    debug_decl(get_authpw, SUDOERS_DEBUG_AUTH);

    if (ISSET(mode, (MODE_CHECK|MODE_LIST))) {
	/* In list mode we always prompt for the user's password. */
	sudo_pw_addref(sudo_user.pw);
	pw = sudo_user.pw;
    } else {
	if (def_rootpw) {
	    if ((pw = sudo_getpwuid(ROOT_UID)) == NULL) {
		log_warningx(SLOG_SEND_MAIL, N_("unknown uid %u"), ROOT_UID);
	    }
	} else if (def_runaspw) {
	    if ((pw = sudo_getpwnam(def_runas_default)) == NULL) {
		log_warningx(SLOG_SEND_MAIL,
		    N_("unknown user %s"), def_runas_default);
	    }
	} else if (def_targetpw) {
	    if (runas_pw->pw_name == NULL) {
		/* This should never be NULL as we fake up the passwd struct */
		log_warningx(SLOG_RAW_MSG, N_("unknown uid %u"),
		    (unsigned int) runas_pw->pw_uid);
	    } else {
		sudo_pw_addref(runas_pw);
		pw = runas_pw;
	    }
	} else {
	    sudo_pw_addref(sudo_user.pw);
	    pw = sudo_user.pw;
	}
    }

    debug_return_ptr(pw);
}

/*
 * Returns true if the specified shell is allowed by /etc/shells, else false.
 */
bool
check_user_shell(const struct passwd *pw)
{
    const char *shell;
    debug_decl(check_user_shell, SUDOERS_DEBUG_AUTH);

    if (!def_runas_check_shell)
	debug_return_bool(true);

    sudo_debug_printf(SUDO_DEBUG_INFO,
	"%s: checking /etc/shells for %s", __func__, pw->pw_shell);

    setusershell();
    while ((shell = getusershell()) != NULL) {
	if (strcmp(shell, pw->pw_shell) == 0)
	    debug_return_bool(true);
    }
    endusershell();

    debug_return_bool(false);
}
