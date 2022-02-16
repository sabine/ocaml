(* TEST
* hasunix
include unix
** bytecode
** native
*)

open Domain

(* This test looks to spawn domains while doing a bunch of explicit minor and
   major GC calls from parallel domains *)

let rec set_gc l =
  if l > 16 then ()
  else
    let g1 = Gc.get() in
      Gc.set { g1 with
        minor_heap_size = ((l mod 4) + 1) * 262144; (*131072; NOTE: this fails*)
        max_domains = ((l mod 2) + 1) * 32; 
      };
      set_gc (l + 1)

let rec burn l =
  if List.hd l > 14 then ()
  else
    burn (l @ l |> List.map (fun x -> x + 1))

let test_parallel_spawn () =
  for i = 1 to 20 do
    let a = Array.init 25 (fun _ -> Domain.spawn (fun () -> burn [0])) in
    for j = 0 to 24 do
      join a.(j)
    done
  done

let () =
  let running = ref true in
  let rec run_until_stop fn () =
    while !running do
      fn ();
    done
  in

  let domain_minor_gc = 
    Domain.spawn (run_until_stop (fun () -> burn [8]; Gc.minor ())) in
  let domain_major_gc = 
    Domain.spawn (run_until_stop (fun () -> burn [8]; Gc.major ())) in
  let domain_set_gc = Domain.spawn (run_until_stop (fun () -> set_gc 1; )) in
  (*let domain_set_gc2 = Domain.spawn (run_until_stop (fun () -> set_gc 3; )) in*)

  test_parallel_spawn ();

  running := false;
  join domain_minor_gc;
  join domain_set_gc;
  join domain_major_gc;
  (*join domain_set_gc2;*) (* two domains using Gc.set fails with segmentation fault on bytecode*)

  print_endline "ok"
