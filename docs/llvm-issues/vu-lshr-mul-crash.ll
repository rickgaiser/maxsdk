target datalayout = "e-m:e-p:16:16-i32:32-i64:64-f32:32-v128:128-n16"
target triple = "vu"

define void @vskin() {
vu.entry:
  %n = tail call i16 @llvm.vu.ilw(i16 0, i32 0)
  %0 = lshr i16 %n, 1
  %.idx = mul i16 %0, 48
  %1 = getelementptr i8, ptr addrspace(4) inttoptr (i16 1216 to ptr addrspace(4)), i16 %.idx
  %2 = addrspacecast ptr addrspace(4) %1 to ptr addrspace(11)
  store <4 x float> zeroinitializer, ptr addrspace(11) %2, align 4
  ret void
}

; Unknown intrinsic
declare i16 @llvm.vu.ilw(i16, i32)
