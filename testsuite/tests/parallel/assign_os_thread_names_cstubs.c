#define CAML_INTERNALS
#include "caml/domain.h"
#include "caml/memory.h"
#include "caml/misc.h"
#include "caml/alloc.h"
#include <stdio.h>

CAMLprim value get_correct_thread_name()
{
  CAMLparam0();
  CAMLlocal1(result);

  char domain_id_suffix[MAX_OS_THREAD_NAME_LENGTH - 1] = "";
  char thread_name[MAX_OS_THREAD_NAME_LENGTH];

  snprintf(domain_id_suffix, MAX_OS_THREAD_NAME_LENGTH - 1, ":%x",
          Caml_state->unique_id);
  snprintf(thread_name, MAX_OS_THREAD_NAME_LENGTH, "%*.*s%s",
          0, MAX_OS_THREAD_NAME_LENGTH  - 1 - (int)strlen(domain_id_suffix),
          caml_os_thread_name, domain_id_suffix);

  result = caml_copy_string(thread_name);
  CAMLreturn(result);
}

CAMLprim value thread_getname()
{
  CAMLparam0();
  CAMLlocal1(result);
  char tmp[MAX_OS_THREAD_NAME_LENGTH];
  caml_os_thread_getname(tmp);
  result = caml_copy_string(tmp);
  CAMLreturn(result);
}
