#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>

#include "php_virtual_cwd.h"

#ifdef ZTS
#include "TSRM.h"
#endif

ZEND_DECLARE_MODULE_GLOBALS(cwd);

cwd_state true_global_cwd_state;

#ifndef ZEND_WIN32
#include <unistd.h>
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) ((mode) & _S_IFDIR)
#endif

#ifndef S_ISREG
#define S_ISREG(mode) ((mode) & _S_IFREG)
#endif

#ifdef ZEND_WIN32
#define strtok_r(a,b,c) strtok((a),(b))
#define IS_SLASH(c)	((c) == '/' || (c) == '\\')
#define DEFAULT_SLASH '\\'
#define TOKENIZER_STRING "/\\"

#define IS_ABSOLUTE_PATH(path, len) \
	(len >= 2 && isalpha(path[0]) && path[1] == ':')

#define COPY_WHEN_ABSOLUTE 2
	
static int php_check_dots(const char *element, int n) 
{
	while (n-- > 0) if (element[n] != '.') break;

	return (n != -1);
}
	
#define IS_DIRECTORY_UP(element, len) \
	(len >= 2 && !php_check_dots(element, len))

#define IS_DIRECTORY_CURRENT(element, len) \
	(len == 1 && ptr[0] == '.')


#else
#define IS_SLASH(c)	((c) == '/')
#define DEFAULT_SLASH '/'
#define TOKENIZER_STRING "/"
#endif


/* default macros */

#ifndef IS_ABSOLUTE_PATH	
#define IS_ABSOLUTE_PATH(path, len) \
	(IS_SLASH(path[0]))
#endif

#ifndef IS_DIRECTORY_UP
#define IS_DIRECTORY_UP(element, len) \
	(len == 2 && memcmp(element, "..", 2) == 0)
#endif

#ifndef IS_DIRECTORY_CURRENT
#define IS_DIRECTORY_CURRENT(element, len) \
	(len == 1 && ptr[0] == '.')
#endif

#ifndef COPY_WHEN_ABSOLUTE
#define COPY_WHEN_ABSOLUTE 0
#endif

/* define this to check semantics */
#define IS_DIR_OK(s) (1)
	
#ifndef IS_DIR_OK
#define IS_DIR_OK(state) (php_is_dir_ok(state) == 0)
#endif
	
static int php_is_dir_ok(const cwd_state *state) 
{
	struct stat buf;

	if (stat(state->cwd, &buf) == 0 && S_ISDIR(buf.st_mode))
		return (0);

	return (1);
}

static int php_is_file_ok(const cwd_state *state) 
{
	struct stat buf;

	if (stat(state->cwd, &buf) == 0 && S_ISREG(buf.st_mode))
		return (0);

	return (1);
}

static void cwd_globals_ctor(zend_cwd_globals *cwd_globals)
{
	cwd_globals->cwd.cwd = (char *) malloc(true_global_cwd_state.cwd_length+1);
	memcpy(cwd_globals->cwd.cwd, true_global_cwd_state.cwd, true_global_cwd_state.cwd_length+1);
	cwd_globals->cwd.cwd_length = true_global_cwd_state.cwd_length;
}

void virtual_cwd_startup()
{
	char cwd[1024]; /* Should probably use system define here */
	char *result;

	result = getcwd(cwd, sizeof(cwd));	
	if (!result) {
		cwd[0] = '\0';
	}
	true_global_cwd_state.cwd = strdup(cwd);
	true_global_cwd_state.cwd_length = strlen(cwd);

	ZEND_INIT_MODULE_GLOBALS(cwd, cwd_globals_ctor, NULL);
}

char *virtual_getcwd_ex(int *length)
{
	cwd_state *state;
	CWDLS_FETCH();

	state = &CWDG(cwd);

	if (state->cwd_length == 0) {
		char *retval;

		*length = 1;
		retval = (char *) malloc(2);
		retval[0] = DEFAULT_SLASH;
		retval[1] = '\0';	
		return retval;
	}

#ifdef ZEND_WIN32
	/* If we have something like C: */
	if (state->cwd_length == 2 && state->cwd[state->cwd_length-1] == ':') {
		char *retval;

		*length = state->cwd_length+1;
		retval = (char *) malloc(*length+1);
		memcpy(retval, state->cwd, *length);
		retval[*length-1] = DEFAULT_SLASH;
		retval[*length] = '\0';
		return retval;
	}
#endif
	*length = state->cwd_length;
	return strdup(state->cwd);
}


/* Same semantics as UNIX getcwd() */
char *virtual_getcwd(char *buf, size_t size)
{
	int length;
	char *cwd;

	cwd = virtual_getcwd_ex(&length);

	if (buf == NULL) {
		return cwd;
	}
	if (length > size-1) {
		free(cwd);
		errno = ERANGE; /* Is this OK? */
		return NULL;
	}
	memcpy(buf, cwd, length+1);
	free(cwd);
	return buf;
}


/* returns 0 for ok, 1 for error */
int virtual_file_ex(cwd_state *state, char *path, verify_path_func verify_path)
{
	int path_length = strlen(path);
	char *ptr = path;
	char *tok = NULL;
	int ptr_length;
	cwd_state *old_state;
	int ret = 0;
	int copy_amount = -1;

	if (path_length == 0) 
		return (0);

	old_state = (cwd_state *) malloc(sizeof(*old_state));
	old_state->cwd = strdup(state->cwd);
	old_state->cwd_length = state->cwd_length;

	if (IS_ABSOLUTE_PATH(path, path_length)) {
		copy_amount = COPY_WHEN_ABSOLUTE;
#ifdef ZEND_WIN32
	} else if(IS_SLASH(path[0])) {
		copy_amount = 2;
#endif
	}

	if (copy_amount != -1) {
		state->cwd = (char *) realloc(state->cwd, copy_amount + 1);
		if (copy_amount)
			memcpy(state->cwd, old_state->cwd, copy_amount);
		state->cwd[copy_amount] = '\0';
		state->cwd_length = copy_amount;
		path += copy_amount;
	}


	ptr = strtok_r(path, TOKENIZER_STRING, &tok);
	while (ptr) {
		ptr_length = strlen(ptr);

		if (IS_DIRECTORY_UP(ptr, ptr_length)) {
			char save;

			save = DEFAULT_SLASH;

#define PREVIOUS state->cwd[state->cwd_length - 1]

			while (IS_ABSOLUTE_PATH(state->cwd, state->cwd_length) &&
					!IS_SLASH(PREVIOUS)) {
				save = PREVIOUS;
				PREVIOUS = '\0';
				state->cwd_length--;
			}

			if (!IS_ABSOLUTE_PATH(state->cwd, state->cwd_length)) {
				state->cwd[state->cwd_length++] = save;
				state->cwd[state->cwd_length] = '\0';
			} else {
				PREVIOUS = '\0';
				state->cwd_length--;
			}
		} else if (!IS_DIRECTORY_CURRENT(ptr, ptr_length)) {
			state->cwd = (char *) realloc(state->cwd, state->cwd_length+ptr_length+1+1);
			state->cwd[state->cwd_length] = DEFAULT_SLASH;
			memcpy(&state->cwd[state->cwd_length+1], ptr, ptr_length+1);
			state->cwd_length += (ptr_length+1);
		}
		ptr = strtok_r(NULL, TOKENIZER_STRING, &tok);
	}

	if (verify_path && !verify_path(state)) {
		free(state->cwd);

		*state = *old_state;

		ret = 1;
	} else {
		free(old_state->cwd);
	}
	
	free(old_state);
	
	return (ret);
}

int virtual_chdir(char *path)
{
	CWDLS_FETCH();

	return virtual_file_ex(&CWDG(cwd), path, php_is_dir_ok);
}

int virtual_filepath(char *path, char **filepath)
{
	cwd_state new_state;
	int retval;
	CWDLS_FETCH();

	new_state = CWDG(cwd);
	new_state.cwd = strdup(CWDG(cwd).cwd);

	retval = virtual_file_ex(&new_state, path, php_is_file_ok);
	*filepath = new_state.cwd;
	return retval;
}

FILE *virtual_fopen(char *path, const char *mode)
{
	cwd_state new_state;
	FILE *f;
	int retval;
	CWDLS_FETCH();

	new_state = CWDG(cwd);
	new_state.cwd = strdup(CWDG(cwd).cwd);

	retval = virtual_file_ex(&new_state, path, php_is_file_ok);

	if (retval) {
		return NULL;
	}
	f = fopen(new_state.cwd, mode);
	free(new_state.cwd);
	return f;
}

#if 0

main(void)
{
	cwd_state state;
	int length;

#ifndef ZEND_WIN32
	state.cwd = malloc(PATH_MAX + 1);
	state.cwd_length = PATH_MAX;

	while (getcwd(state.cwd, state.cwd_length) == NULL && errno == ERANGE) { 
		state.cwd_length <<= 1;
		state.cwd = realloc(state.cwd, state.cwd_length + 1);
	}
#else
	state.cwd = strdup("d:\\foo\\bar");
#endif
	state.cwd_length = strlen(state.cwd);

#define T(a) \
	printf("[%s] $ cd %s\n", virtual_getcwd_ex(&state, &length), a); \
	virtual_chdir(&state, strdup(a)); \
	printf("new path is %s\n", virtual_getcwd_ex(&state, &length));
	
	T("..")
	T("...")
	T("foo")
	T("../bar")
	T(".../slash/../dot")
	T("//baz")
	T("andi/././././././///bar")
	T("../andi/../bar")
	T("...foo")
	T("D:/flash/zone")
	T("../foo/bar/../baz")

	return 0;
}

#endif
