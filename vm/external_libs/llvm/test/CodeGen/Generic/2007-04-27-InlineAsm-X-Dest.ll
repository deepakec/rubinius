; RUN: llvm-as < %s | llc
; XFAIL: sparc-sun-solaris2
; PR1557

; Test that we can have an "X" output constraint.

define void @test(i16 * %t) {
        call void asm sideeffect "foo $0", "=*X,~{dirflag},~{fpsr},~{flags},~{memory}"( i16* %t )
        ret void
}
