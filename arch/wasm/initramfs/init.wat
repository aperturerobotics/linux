(module
  ;; Import syscall handler from host.
  ;; Signature: (syscall_nr, arg0, arg1, arg2) -> result
  (import "env" "host_syscall" (func $host_syscall (param i32 i32 i32 i32) (result i32)))

  ;; Own memory for the message string.
  (memory (export "memory") 1)
  (data (i32.const 0) "Hello from userspace!\n")

  (func (export "_start")
    ;; sys_write(fd=1, buf=0, len=22)
    ;; Linux ARM/generic syscall numbers: write=64, exit=93
    (drop (call $host_syscall (i32.const 64) (i32.const 1) (i32.const 0) (i32.const 22)))
    ;; sys_exit_group(status=0), nr=94
    (drop (call $host_syscall (i32.const 94) (i32.const 0) (i32.const 0) (i32.const 0)))
    unreachable
  )
)
