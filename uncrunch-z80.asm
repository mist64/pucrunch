
;*
;*
;* PUCRUNCH unpacker for GB
;*   Modeled after Pasi Ojala's C64 code.
;*
;*   Written in RGBDS
;*
;*  V1.0 - Ported to GB by Jeff Frohwein, started 22-Jul-99
;*  V1.1 - Various optimizations, 23-Jul-99
;*  V1.2 - Even more optimizations, 23-Jul-99
;*  V1.3 - Fixed a bug in the code. 256 byte copy didn't work. 24-Feb-00
;*
;* Note: If you unpack to VRAM than the screen needs to be
;* turned off because no checks for VRAM available are made.

; Pucrunch file format

;;; db INPOS low     (endAddr + overlap - size) & 0xff
;;; db INPOS high    (endAddr + overlap - size) >> 8
;;; db 'p'
;;; db 'u'
;;; db (endAddr - 0x100) & 0xff
;;; db (endAddr - 0x100) >> 8
; db escape>>(8-escBits)
;;; db (start & 0xff) (OUTPOS low)
;;; db (start >> 8) (OUTPOS high)
; db escBits
; db maxGamma + 1
; db (1<<maxGamma); /* Short/Long RLE */
; db extraLZPosBits;
;;; db (exec & 0xff)
;;; db (exec >> 8)
; db rleUsed (31)  ;needed
; ds rleUsed
;  ....data....

        PUSHS

        SECTION "Pucrunch Vars",BSS

;bitstr          DB
escPu           DB
;InPtr           DW
OutPtr          DW
;regx            DB
;regy            DB
lzpos           DW
EscBits         DB
Esc8Bits        DB
MaxGamma        DB
Max1Gamma       DB
Max2Gamma       DB
Max8Gamma       DB
ExtraBits       DB
tablePu         DS      31


        SECTION "Pucrunch High Vars",HRAM

regy            DB

        POPS

; HL = InPtr
; D = bitstr
; E = X
; BC = temps

; ****** Unpack pucrunch data ******
; Entry: HL = Source packed data
;        DE = Destination for unpacked data

Unpack:
        ld16r   OutPtr,de

; Read the file header & setup variables

        ld      bc,6
        add     hl,bc

        ld      a,[hl+]
        ld      [escPu],a

        inc     hl
        inc     hl

        ld      a,[hl+]
        ld      [EscBits],a
        ld      b,a

        ld      a,8
        sub     b
        ld      [Esc8Bits],a

        ld      a,[hl+]
        ld      [MaxGamma],a
        dec     a
        ld      b,a
        ld      a,8
        sub     b
        ld      [Max8Gamma],a

        ld      a,[hl+]
        ld      [Max1Gamma],a
        add     a
        dec     a
        ld      [Max2Gamma],a

        ld      a,[hl+]
        ld      [ExtraBits],a

        inc     hl
        inc     hl

        ld      a,[hl+]
        ld      b,a

        ld      de,tablePu

; Copy the RLE table (maximum of 31 bytes) to RAM

        inc     b
        srl     b
        jr      nc,.orleloop

.rleloop:
        ld      a,[hl+]
        ld      [de],a
        inc     de

.orleloop:
        ld      a,[hl+]
        ld      [de],a
        inc     de

        dec     b
        jr      nz,.rleloop



        ld      d,$80

        jr      .main


.newesc:
        ld      b,a

        ld      a,[escPu]
        ld      [regy],a

        ld      a,[EscBits]
        ld      e,a

        ld      a,b

        inc     e

        call    .getchk

        ld      [escPu],a

        ld      a,[regy]

        ; Fall through and get the rest of the bits.

.noesc:
        ld      b,a

        ld      a,[Esc8Bits]
        ld      e,a

        ld      a,b

        inc     e

        call    .getchk

; Write out the escaped/normal byte

        ld16    bc,OutPtr
        ld      [bc],a
        inc     bc
        ld16r   OutPtr,bc

       ; Fall through and check the escape bits again

.main:
        ld      a,[EscBits]
        ld      e,a

        xor     a               ; A = 0
        ld      [regy],a

        inc     e

        call    .getchk         ; X=2 -> X=0

        ld      b,a
        ld      a,[escPu]
        cp      b
        ld      a,b

        jr      nz,.noesc       ; Not the escape code -> get the rest of the byte

        ; Fall through to packed code

        call    .getval         ; X=0 -> X=0

        ld      [lzpos],a       ; xstore - save the length for a later time

        srl     a               ; cmp #1        ; LEN == 2 ? (A is never 0)
        jp      nz,.lz77        ; LEN != 2      -> LZ77

        call    .get1bit        ; X=0 -> X=0

        srl     a               ; bit -> C, A = 0

        jp      nc,.lz77_2      ; A=0 -> LZPOS+1        LZ77, len=2

	; e..e01
        call    .get1bit        ; X=0 -> X=0
        srl     a               ; bit -> C, A = 0
        jp      nc,.newesc      ; e..e010               New Escape

	; e..e011				Short/Long RLE
        ld      a,[regy]        ; Y is 1 bigger than MSB loops
        inc     a
        ld      [regy],a

        call    .getval         ; Y is 1, get len,  X=0 -> X=0
        ld      [lzpos],a       ; xstore - Save length LSB

        ld      c,a

        ld      a,[Max1Gamma]
        ld      b,a

        ld      a,c

        cp      b               ; ** PARAMETER 63-64 -> C set, 64-64 -> C clear..

        jr      c,.chrcode      ; short RLE, get bytecode

	; Otherwise it's long RLE
.longrle:
        ld      b,a
        ld      a,[Max8Gamma]
        ld      e,a             ; ** PARAMETER  111111xxxxxx
        ld      a,b

        call    .getbits        ; get 3/2/1 more bits to get a full byte,  X=2 -> X=0
        ld      [lzpos],a       ; xstore - Save length LSB

        call    .getval         ; length MSB, X=0 -> X=0

        ld      [regy],a        ; Y is 1 bigger than MSB loops

.chrcode:
        call    .getval         ; Byte Code,  X=0 -> X=0

        ld      e,a

        ld      c,(tablePu-1)%256
        add     c
        ld      c,a
        ld      a,(tablePu-1)/256
        adc     0
        ld      b,a

        ld      a,e
        cp      32              ; 31-32 -> C set, 32-32 -> C clear..
        ld      a,[bc]
        jr      c,.less32       ; 1..31

	; Not ranks 1..31, -> 11111°xxxxx (32..64), get byte..

        ld      a,e        ; get back the value (5 valid bits)

        ld      e,3

        call    .getbits        ; get 3 more bits to get a full byte, X=3 -> X=0

.less32:
        push    hl
        push    af

        ld      a,[lzpos]
        ld      e,a          ; xstore - get length LSB

        ld      b,e
        inc     b               ; adjust for cpx#$ff;bne -> bne

        ld      a,[regy]
        ld      c,a

        ld16r   hl,OutPtr

        pop     af

.dorle:
        ld      [hl+],a

        dec     b
        jr      nz,.dorle       ; xstore 0..255 -> 1..256

        dec     c
        jr      nz,.dorle       ; Y was 1 bigger than wanted originally

        ld16r   OutPtr,hl

        pop     hl
        jp      .main

.lz77:
        call    .getval         ; X=0 -> X=0

        ld      b,a

        ld      a,[Max2Gamma]
        cp      b               ; end of file ?
        ret     z               ; yes, exit

        ld      a,[ExtraBits]   ; ** PARAMETER (more bits to get)
        ld      e,a

        ld      a,b

        dec     a               ; subtract 1  (1..126 -> 0..125)

        inc     e

        call    .getchk ;f        ; clears Carry, X=0 -> X=0

.lz77_2:
        ld      [lzpos+1],a     ; offset MSB

        ld      e,8

        call    .getbits        ; clears Carry, X=8 -> X=0

                        ; Note: Already eor:ed in the compressor..
        ld      b,a

        ld      a,[lzpos]
        ld      e,a             ; xstore - LZLEN (read before it's overwritten)

        ld      a,[OutPtr]
        add     b               ; -offset -1 + curpos (C is clear)
        ld      [lzpos],a

        ld      a,[lzpos+1]
        ld      b,a

        ld      a,[OutPtr+1]
        ccf
        sbc     b
        ld      [lzpos+1],a     ; copy X+1 number of chars from LZPOS to OUTPOS

        inc     e               ; adjust for cpx#$ff;bne -> bne

; Write decompressed bytes out to RAM
        ld      b,e

        push    de
        push    hl

        ld16r   hl,lzpos
        ld16r   de,OutPtr

        ld      a,b
        or      a               ; Is it zero?
        jr      z,.zero         ; yes

        inc     b
        srl     b
        jr      nc,.olzloop

.lzloop:
        ld      a,[hl+]         ; Note: Must be copied forward
        ld      [de],a
        inc     de
.olzloop:
        ld      a,[hl+]         ; Note: Must be copied forward
        ld      [de],a
        inc     de

        dec     b
        jr      nz,.lzloop      ; X loops, (256,1..255)

        ld16r   OutPtr,de

        pop     hl
        pop     de
        jp      .main

.zero:
        ld      b,128
        jr      .lzloop

; getval : Gets a 'static huffman coded' value
; ** Scratches X, returns the value in A **
.getval:                        ; X must be 0 when called!
        ld      a,1
        ld      e,a
.loop0:
        sla     d

        jr      nz,.loop1

        ld      d,[hl]
        inc     hl

        rl      d               ; Shift in C=1 (last bit marker)
                                ; bitstr initial value = $80 == empty
.loop1:
        jr      nc,.getchk      ; got 0-bit

        inc     e

        ld      b,a             ; save a

        ld      a,[MaxGamma]
        cp      e

        ld      a,b             ; restore a

        jr      nz,.loop0

        jr      .getchk


; getbits: Gets X bits from the stream
; ** Scratches X, returns the value in A **

.get1bit:
        inc     e
.getbits:
        sla     d

        jr      nz,.loop3

        ld      d,[hl]
        inc     hl

        rl      d               ; Shift in C=1 (last bit marker)
                                ; bitstr initial value = $80 == empty
.loop3:
        rla
.getchk:
        dec     e

        jr      nz,.getbits
        or      a       ; clear carry flag
        ret