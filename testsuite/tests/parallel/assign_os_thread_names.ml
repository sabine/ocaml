(* TEST
  modules = "assign_os_thread_names_cstubs.c"
  ocamlrunparam += ",T=1"
* hasunix
include unix
** bytecode
** native
*)

open Domain

external get_correct_thread_name : unit -> string = "get_correct_thread_name"
external thread_getname : unit -> string = "thread_getname"


let check_get_name () =
  let correct_name = get_correct_thread_name () in
  let thread_name = thread_getname () in
  if compare correct_name thread_name == 0 then
    (Printf.printf "ok\n")
  else
    (Printf.printf "%s != %s\n" thread_name correct_name)


let spawn_and_print () =
  check_get_name ();
  let d = Domain.spawn check_get_name in
  join d

let set_name_and_spawn () =
  check_get_name ();
  let d = Domain.spawn check_get_name in
  join d

let set_name_and_double_spawn () =
  check_get_name ();
  let d = Domain.spawn spawn_and_print in
  join d

let () =
  spawn_and_print ();

  let d = Domain.spawn set_name_and_spawn in
  join d;

  let d = Domain.spawn set_name_and_double_spawn in
  join d
