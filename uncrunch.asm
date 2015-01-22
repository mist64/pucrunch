;$a533 relink lines
;$a871 run
; if Z=1, calls a659: a68e [settextptr], ffe7 [clall], varptrs,
;	a81d [restore], stack set
;$a7ae	42926	newstt	BASIC Warm Start


;DLZ = 1	; Delta LZ77 -- add offset
;DLZ = 0

;FAST  = 1	; A little faster, but a little longer
;FAST  = -1	; Slower, but shorter
;WRAP  = 1	; wrap buffer enabled

C64 = 0
VIC20 = 1
C16 = 2
C128 = 3

#if MACH = C64
#if FAST = 0
#else
#if FAST = 1
#else
#if FAST = -1
#else
	err invalid speed option..
#endif	; -1
#endif	; 1
#endif	; 0
#else
#if MACH = VIC20
;#if FAST = 0
;#else
;	err invalid speed option..
;#endif	; 0
#else
#if MACH = C16
#if FAST = 0
#else
	err invalid speed option..
#endif	; 0
#else
#if MACH = C128
#else
	err invalid machine
#endif	; C128
#endif	; C16
#endif	; VIC20
#endif	; C64


#if FAST = -1 && (MACH = C64 || MACH = VIC20)

;#if MACH = VIC20
;#if WRAP
;	err -fshort and wrap not possible with VIC20
;#endif
;#endif

#if DLZ = 1
	err DLZ not valid in -fshort
#endif
	processor 6502
	; Note: Possibilities for shortening
	;	- No basic end address set:   8 bytes
	;	- No 2 MHz mode set/reset:    6 bytes
	;	- No settable memory config:  2 bytes
	;	- No basic line:             12 bytes
	;	- No RLE-code:		     12 bytes + max 31 table bytes
	;	- No RLE at all:	     50 bytes + max 31 table bytes
#if MACH = C128
BASEND	EQU $1210	; start of basic variables
LZPOS	EQU $a3
#else
BASEND	EQU $2d		; start of basic variables
LZPOS	EQU $2f
#endif

bitstr	EQU $f7		; Hint the value beforehand
#if WRAP
#if (MACH = VIC20 || MACH = C16)
wrap	EQU $f9		; Hint the value beforehand
#endif
WRAPBUF	EQU $004b	; 'wrap' buffer, 22 bytes ($02a7 for 89 bytes)
#endif

#if MACH = C64
	ORG $0801
	DC.B $0b,8,$ef,0	; '239 SYS2061'
	DC.B $9e,$32,$30,$36
	DC.B $31,0,0,0
#endif
#if MACH = VIC20
	ORG $1201
	DC.B $0b,$12,$ef,0	; '239 SYS4621'
	DC.B $9e,$34,$36,$32
	DC.B $31,0,0,0
#endif

#if MACH = C64
	sei
	lda #$38
	sta 1
#endif
	lda #$aa	;+1fTBEndLo	** Set the basic prg end address
	sta BASEND
	lda #$aa	;+1ftBEndHi	**
	sta BASEND+1

#if WRAP
	ldx #0		;+1ftOverlap		** parameter - # of overlap bytes-1 off $ffff
overlap	lda $aaaa,x	;+1ftOverlapLo+2ftOverlapHi	** parameter start of off-end bytes
	sta WRAPBUF,x
	dex
	bpl overlap
#if MACH != C64
	txs		; use stack from $ff downwards
#endif
#endif

	ldx #block_stack_end-block_stack+1	;+1ftStackSize
packlp2	lda block_stack-1,x	;+2ftReloc
	dc.b $9d		; sta $nnnn,x
	dc.w block_stack_-1	; (ZP addressing only addresses ZP!)
	dex
	bne packlp2

	; Max 255 extra bytes are copied, but they are at the start
	; of the data, i.e. the decompressor.

	; Note that we would overwrite this copy loop in extreme cases
	; if we didn't move the data further up in memory to prevent it.

	ldy #$aa	;+1ftSizePages		** parameter SIZE high + 1
cploop	dex		; ldx #$ff on the first round
	lda $aaaa,x	;+1ftSizeLo+2ftSizeHi	** parameter DATAEND-0x100
	sta $ff00,x	;+1ftEndLo+2ftEndHi	** parameter ORIG LEN-0x100+ reserved bytes
	txa		;cpx #0
	bne cploop
	dec cploop+6	;+2ftReloc
	dec cploop+3	;+2ftReloc
	dey
	bne cploop
	jmp main	;+0ftDeCall



block_stack
#rorg $f7	; $f7 - ~$1e0
block_stack_

bitstr	dc.b $80	; ZP	$80 == Empty
esc	dc.b $00	;+0ftEscValue		** parameter (saves a byte when here)
#if MACH = VIC20 || MACH = C16
#if WRAP
wrap	dc.b $ff	;+0ftWrapCount	** parameter - 'memory size' in pages + N
#endif
#endif

OUTPOS = *+1		; ZP
putch	sta $aaaa	;+1ftOutposLo+2ftOutposHi	** parameter
	inc OUTPOS	; ZP
	bne 0$
	inc OUTPOS+1	; ZP
0$	dex
	rts

newesc	ldy esc		; remember the old code (top bits for escaped byte)
	ldx #2		;+1ftEscBits	** PARAMETER
	jsr getchkf	; get & save the new escape code (allows X=0)
	sta esc
	tya		; pre-set the bits
	; Fall through and get the rest of the bits.
noesc	ldx #6		;+1ftEsc8Bits	** PARAMETER
	jsr getchkf
	jsr putch	; output the escaped/normal byte
	; Fall through and check the escape bits again
main	ldy #0		; Reset to a defined state
	tya		; A = 0
	ldx #2		;+1ftEscBits	** PARAMETER number of escape bits (allows X=0)
	jsr getchkf	; X=2 -> X=0
	cmp esc
	bne noesc	; Not the escape code -> get the rest of the byte
	; Fall through to packed code

	jsr getval	; X=0 -> X=0
	sta LZPOS	; xstore - save the length for a later time
	lsr		; cmp #1	; LEN == 2 ? (A is never 0)
	bne lz77	; LEN != 2	-> LZ77
	;tya		; A = 0

	jsr getbit	; X=0 -> X=0
	bcc lz77_2	; A=0 -> LZPOS+1	LZ77, len=2
	; e..e01
	jsr getbit	; X=0 -> X=0
	bcc newesc	; e..e010		New Escape

	; e..e011				Short/Long RLE
	iny		; Y is 1 bigger than MSB loops
	jsr getval	; Y is 1, get len,  X=0 -> X=0
	sta LZPOS	; xstore - Save length LSB
	cmp #64		;+1ft1MaxGamma	** PARAMETER 63-64 -> C clear, 64-64 -> C set..
	bcc chrcode	; short RLE, get bytecode
	; Otherwise it's long RLE
longrle	ldx #2		;+1ft8MaxGamma	** PARAMETER	111111xxxxxx
	jsr getbits	; get 3/2/1 more bits to get a full byte,  X=2 -> X=0
	sta LZPOS	; xstore - Save length LSB

	jsr getval	; length MSB, X=0 -> X=0
	tay		; Y is 1 bigger than MSB loops

chrcode	jsr getval	; Byte Code,  X=0 -> X=0
	tax		; this is executed most of the time anyway
	lda table-1,x	; Saves one jump if done here (loses one txa)

	cpx #16 ;32	; 31-32 -> C clear, 32-32 -> C set..
	bcc 1$		; 1..31

	; Not ranks 1..31, -> 11111°xxxxx (32..64), get byte..
	txa		; get back the value (5 valid bits)
	ldx #4	;3
	jsr getbits	; get 3 more bits to get a full byte, X=3 -> X=0

1$	ldx LZPOS	; xstore - get length LSB
	inx		; adjust for cpx#$ff;bne -> bne
dorle	jsr putch	;+dex
	bne dorle	; xstore 0..255 -> 1..256
	dey
	bne dorle	; Y was 1 bigger than wanted originally
mainbeq	beq main	; reverse condition -> jump always


lz77	jsr getval	; X=0 -> X=0
	cmp #127	;+1ft2MaxGamma	** PARAMETER	Clears carry (is maximum value)
	beq eof		; EOF

	sbc #0		; C is clear -> subtract 1  (1..126 -> 0..125)
	ldx #0		;+1ftExtraBits	** PARAMETER (more bits to get)
	jsr getchkf	; clears Carry, X=0 -> X=0

lz77_2	sta LZPOS+1	; offset MSB
	ldx #8
	jsr getbits	; clears Carry, X=8 -> X=0
			; Note: Already eor:ed in the compressor..
	;eor #255	; offset LSB 2's complement -1 (i.e. -X = ~X+1)
	adc OUTPOS	; -offset -1 + curpos (C is clear)
	ldx LZPOS	; xstore - LZLEN (read before it's overwritten)

	sta LZPOS
	lda OUTPOS+1
	sbc LZPOS+1	; takes C into account
	sta LZPOS+1	; copy X+1 number of chars from LZPOS to OUTPOS
	;ldy #0		; Y was 0 originally, we don't change it
	inx		; adjust for cpx#$ff;bne -> bne
lzloop	lda (LZPOS),y	; Note: *Must* be copied forwards!
	iny		; Y does not wrap because X=0..255 and Y initially 0
	jsr putch	;+dex
	bne lzloop	; X loops, (256,1..255)
	beq mainbeq	; jump through another beq (-1 byte, +3 cycles)

	; EOF
eof
#if MACH = C64
	lda #$37	;+1ftMemConfig		** PARAMETER
	sta 1
	cli		;+0ftCli		** PARAMETER
#endif
#if BASIC == 1
#if MACH == C64
	;jsr $a533	; relink lines
	lda #0	; set Z
	jsr $a871	; run
	jmp $a7ae	; basic warm start
#endif
#if MACH == VIC20
	;jsr $c533	; relink lines
	lda #0	; set Z
	jsr $c871	; run
	jmp $c7ae	; basic warm start
#endif
#else
	jmp $aaaa	;+1ftExecLo+2ftExecHi	** PARAMETER
#endif


getbit	asl bitstr
	bne gbend
	pha		; 1 Byte/3 cycles
INPOS = *+1
	lda $aaaa	;+1ftInposLo+2ftInposHi	** parameter
	rol		; Shift in C=1 (last bit marker)
	sta bitstr	; bitstr initial value = $80 == empty

	inc INPOS	; Does not change C!
	bne 0$
	inc INPOS+1	; Does not change C!
#if WRAP
#if MACH = C64
	bne 0$
#else
	dec wrap
	bne 0$
#endif
	; This code does not change C!
	lda #WRAPBUF	; Wrap from $ffff->$0000 -> WRAPBUF
	sta INPOS
#if MACH = C64
	;lda #>WRAPBUF
	;sta INPOS+1
#else
	lda #0
	sta INPOS+1
#endif
#endif
0$	pla		; 1 Byte/4 cycles
gbend	rts


; getval : Gets a 'static huffman coded' value
; Scratches X, returns the value in A **
getval	inx		; X must be 0 when called!
	txa		; set the top bit (value is 1..255)
0$	jsr getbit
	bcc getchk	; got 0-bit
	inx
	cpx #7		;+1ftMaxGamma	** parameter
	bne 0$
	beq getchk	; inverse condition -> jump always

getbits	jsr getbit
	rol		;2
getchk	dex		;2		more bits to get ?
getchkf	bne getbits	;2/3
	clc		;2		return carry cleared
	rts		;6+6


table	dc.b 0,0,0,0,0,0,0	; the table must be at the end of the file!
	dc.b 0,0,0,0,0,0,0,0
	;dc.b 0,0,0,0,0,0,0,0
	;dc.b 0,0,0,0,0,0,0,0

#rend
block_stack_end


; *************************************************************************

#else
	processor 6502
	; Note: Possibilities for shortening
	;	- No basic end address set:   8 bytes
	;	- No 2 MHz mode set/reset:    6 bytes
	;	- Complete stack overwrite:  ~10 bytes
	;	- No settable memory config:  2 bytes
	;	- No basic line:             12 bytes
	;	- No RLE-code:		     12 bytes + max 31 table bytes

#if MACH = C128
BASEND	EQU $1210	; start of basic variables (updated at EOF)
LZPOS	EQU $a3
#else
BASEND	EQU $2d		; start of basic variables (updated at EOF)
LZPOS	EQU $2d		; temporary, BASEND *MUST* *BE* *UPDATED* at EOF
#endif

bitstr	EQU $f7		; Hint the value beforehand
#if MACH = VIC20 || MACH = C16
#if WRAP
wrap	EQU $f9		; Hint the value beforehand
#endif
#endif


#if WRAP
WRAPBUF	EQU $004b	; 'wrap' buffer, 22 bytes ($02a7 for 89 bytes)
#endif

#if MACH = C64
	ORG $0801
	DC.B $0b,8,$ef,0	; '239 SYS2061'
	DC.B $9e,$32,$30,$36
	DC.B $31,0,0,0
#endif
#if MACH = VIC20
	ORG $1201
	DC.B $0b,$12,$ef,0	; '239 SYS4621'
	DC.B $9e,$34,$36,$32
	DC.B $31,0,0,0
#endif
#if MACH = C16
	ORG $1001
	DC.B $0b,$10,$ef,0      ; '239 SYS4109'
	DC.B $9e,$34,$31,$30
	DC.B $39,0,0,0
#endif
#if MACH = C128
	ORG $1c01
	DC.B $0b,$1c,$ef,0	; '239 SYS7181'
	DC.B $9e,$37,$31,$38
	DC.B $31,0,0,0
#endif

	sei
#if MACH = C64
	inc $d030	;+0ftFastDisable	** or "bit $d030" if 2MHz mode is not enabled
	lda #$38
	sta 1
#endif
#if MACH = C16
	; clearing $ff06 bit 4, clear $f9 at end
	lda $ff06
	and #$ef
	sta $ff06	;+0ftFastDisable	** blank the screen
	sta $ff3f	;switch off the ROMs
#endif
#if MACH = C128
	lda #$3f
	sta $ff00
#endif


#if WRAP
	ldx #0		;+1ftOverlap		** parameter - # of overlap bytes-1 off $ffff
overlap	lda $aaaa,x	;+1ftOverlapLo+2ftOverlapHi	** parameter start of off-end bytes
	sta WRAPBUF,x
	dex
	bpl overlap
#endif

	ldx #block200_end-block200+1		;+1ftIBufferSize	; $58	($59 max)
packlp	lda block200-1,x	;+2ftReloc
	sta block200_-1,x
	dex
	bne packlp

	ldx #block_stack_end-block_stack+1	;+1ftStackSize	; $b3	(stack! ~$e8 max)
packlp2	lda block_stack-1,x	;+2ftReloc
	dc.b $9d		; sta $nnnn,x
	dc.w block_stack_-1	; (ZP addressing only addresses ZP!)
	dex
	bne packlp2

	ldy #$aa	;+1ftSizePages		** parameter SIZE high + 1 (max 255 extra bytes)
cploop	dex		; ldx #$ff on the first round
	lda $aaaa,x	;+1ftSizeLo+2ftSizeHi	** parameter DATAEND-0x100
	sta $ff00,x	;+1ftEndLo+2ftEndHi	** parameter ORIG LEN-0x100+ reserved bytes
	txa		;cpx #0
	bne cploop
	dec cploop+6	;+2ftReloc
	dec cploop+3	;+2ftReloc
	dey
	bne cploop
	jmp main	;+0ftDeCall



block200
#rorg 	$200	; $200-$258
block200_

getnew	pha		; 1 Byte/3 cycles
INPOS = *+1
	lda $aaaa	;+1ftInposLo+2ftInposHi	** parameter
	rol		; Shift in C=1 (last bit marker)
	sta bitstr	; bitstr initial value = $80 == empty

	inc INPOS	; Does not change C!
	bne 0$
	inc INPOS+1	; Does not change C!
#if WRAP
#if MACH = C64 || MACH = C128	; Wrap at end of mem..
	bne 0$
#else
	dec wrap	; Does not change C!
	bne 0$
#endif
	; This code does not change C!
	lda #WRAPBUF	; Wrap from $ffff->$0000 -> WRAPBUF
	sta INPOS
#if MACH = C64
	;lda #>WRAPBUF
	;sta INPOS+1
#else
	lda #0
	sta INPOS+1
#endif
#endif
0$	pla		; 1 Byte/4 cycles
	rts


; getval : Gets a 'static huffman coded' value
; Scratches X, returns the value in A **
getval	inx		; X must be 0 when called!
	txa		; set the top bit (value is 1..255)
0$	asl bitstr	; Note: a space/time tradeoff
	bne 1$		;       the subroutine is only called 1/8
	jsr getnew
1$	bcc getchk	; got 0-bit
	inx
	cpx #7		;+1ftMaxGamma	** parameter
	bne 0$
	beq getchk	; inverse condition -> jump always


; getbits: Gets X bits from the stream
; Scratches X, returns the value in A **
get8bit	ldx #7
get1bit	inx		;2
getbits	asl bitstr	; Note: a space/time tradeoff
	bne 1$		;       the subroutine is only called 1/8
	jsr getnew
1$	rol		;2
getchk	dex		;2		more bits to get ?
getchkf	bne getbits	;2/3
	clc		;2		return carry cleared
	rts		;6+6

#rend
block200_end



block_stack
#rorg $f7	; $f7 - ~$1e0
block_stack_

bitstr	dc.b $80	; ZP	$80 == Empty
esc	dc.b $00	;+0ftEscValue		** parameter (saves a byte when here)
#if MACH = VIC20 || MACH = C16
#if WRAP
wrap	dc.b $ff	;+0ftWrapCount	** parameter - 'memory size' in pages + N
#endif
#endif

OUTPOS = *+1		; ZP
putch	sta $aaaa	;+1ftOutposLo+2ftOutposHi	** parameter
	inc OUTPOS	; ZP
#if FAST = 1	; 8/6 bytes, saves 254 cycles per 256 output bytes (max 0.066s)
	beq 0$
	dex
	rts
0$	inc OUTPOS+1
	dex
	rts
#else
	bne 0$
	inc OUTPOS+1	; ZP
0$	dex
	rts
#endif

newesc	ldy esc		; remember the old code (top bits for escaped byte)
	ldx #2		;+1ftEscBits	** PARAMETER
	jsr getchkf	; get & save the new escape code (allows X=0)
	sta esc
	tya		; pre-set the bits
	; Fall through and get the rest of the bits.
noesc	ldx #6		;+1ftEsc8Bits	** PARAMETER
	jsr getchkf
	jsr putch	; output the escaped/normal byte
	; Fall through and check the escape bits again
main	ldy #0		; Reset to a defined state
#if DLZ = 1
	sty addi+1	;3
#endif
	tya		; A = 0
	ldx #2		;+1ftEscBits	** PARAMETER number of escape bits (allows X=0)
	jsr getchkf	; X=2 -> X=0
	cmp esc
	bne noesc	; Not the escape code -> get the rest of the byte
	; Fall through to packed code

	jsr getval	; X=0 -> X=0
	sta LZPOS	; xstore - save the length for a later time
	lsr		; cmp #1	; LEN == 2 ? (A is never 0)
	bne lz77	; LEN != 2	-> LZ77
	;tya		; A = 0
#if FAST = 1	; 18/12 bytes, ~24 cycles per match (0.7s w/ all 2-byte matches)
	asl bitstr
	bne 1$
	jsr getnew
1$	bcc lz77_2
	asl bitstr
	bne 2$
	jsr getnew
2$	bcc newesc
#else
	jsr get1bit	; X=0 -> X=0
	lsr		; bit -> C, A = 0
	bcc lz77_2	; A=0 -> LZPOS+1	LZ77, len=2
	; e..e01
	jsr get1bit	; X=0 -> X=0
	lsr		; bit -> C, A = 0
	bcc newesc	; e..e010		New Escape
#endif

	; e..e011				Short/Long RLE
	iny		; Y is 1 bigger than MSB loops
	jsr getval	; Y is 1, get len,  X=0 -> X=0
	sta LZPOS	; xstore - Save length LSB
	cmp #64		;+1ft1MaxGamma	** PARAMETER 63-64 -> C clear, 64-64 -> C set..
	bcc chrcode	; short RLE, get bytecode
	; Otherwise it's long RLE
longrle	ldx #2		;+1ft8MaxGamma	** PARAMETER	111111xxxxxx
	jsr getbits	; get 3/2/1 more bits to get a full byte,  X=2 -> X=0
	sta LZPOS	; xstore - Save length LSB

	jsr getval	; length MSB, X=0 -> X=0
	tay		; Y is 1 bigger than MSB loops

chrcode	jsr getval	; Byte Code,  X=0 -> X=0
	tax		; this is executed most of the time anyway
	lda table-1,x	; Saves one jump if done here (loses one txa)

	cpx #16	;32	; 31-32 -> C clear, 32-32 -> C set..
	bcc 1$		; 1..31

	; Not ranks 1..31, -> 11111°xxxxx (32..64), get byte..
	txa		; get back the value (5 valid bits)
	ldx #4	;3
	jsr getbits	; get 3 more bits to get a full byte, X=3 -> X=0

1$	ldx LZPOS	; xstore - get length LSB
	inx		; adjust for cpx#$ff;bne -> bne
dorle	jsr putch	;+dex
	bne dorle	; xstore 0..255 -> 1..256
	dey
	bne dorle	; Y was 1 bigger than wanted originally
mainbeq	beq main	; reverse condition -> jump always


lz77	jsr getval	; X=0 -> X=0
	cmp #127	;+1ft2MaxGamma	** PARAMETER	Clears carry (is maximum value)
	beq eof		; EOF

	sbc #0		; C is clear -> subtract 1  (1..126 -> 0..125)
	ldx #0		;+1ftExtraBits	** PARAMETER (more bits to get)
	jsr getchkf	; clears Carry, X=0 -> X=0

lz77_2	sta LZPOS+1	; offset MSB
	jsr get8bit	; clears Carry, X=8 -> X=0
			; Note: Already eor:ed in the compressor..

	;eor #255	; offset LSB 2's complement -1 (i.e. -X = ~X+1)
	adc OUTPOS	; -offset -1 + curpos (C is clear)
	ldx LZPOS	; xstore - LZLEN (read before it's overwritten)

#if FAST = 1	; 34/20
	sta lzloop+1
	lda OUTPOS+1
	sbc LZPOS+1	; takes C into account
	sta lzloop+2	; copy X+1 number of chars from LZPOS to OUTPOS
	;ldy #0		; Y was 0 originally, we don't change it

	inx		; adjust for cpx#$ff;bne -> bne
lzloop	lda $aaaa,y	; Note: *Must* be copied forwards!
#if DLZ = 1
	clc		;1
addi	adc #0		;2+0ftOp
#endif
	sta (OUTPOS),y
	iny		; Y does not wrap because X=0..255 and Y initially 0
	dex
	bne lzloop	; X loops, (256,1..255)
	dey		; 1  2
	tya		; 1  2
	sec		; 1  2
	adc OUTPOS+0	; 2  3
	sta OUTPOS+0	; 2  3
	bcc 0$		; 2  2/3
	inc OUTPOS+1	; 2  5
0$	jmp main	; 3  3
#else
	sta LZPOS
	lda OUTPOS+1
	sbc LZPOS+1	; takes C into account
	sta LZPOS+1	; copy X+1 number of chars from LZPOS to OUTPOS
	;ldy #0		; Y was 0 originally, we don't change it

	inx		; adjust for cpx#$ff;bne -> bne
lzloop	lda (LZPOS),y	; Note: *Must* be copied forwards!
	; Note: lda $nnnn,y would be 3 bytes longer, but only 1 cycle faster
#if DLZ = 1
	clc		;1
addi	adc #0		;2+0ftOp
#endif
	iny		; Y does not wrap because X=0..255 and Y initially 0
	jsr putch	;+dex
	; Note: sta (OUTPOS),y would be quite a bit faster (-17),
	;       but OUTPOS update would make the code longer (+11 bytes)
	bne lzloop	; X loops, (256,1..255)
	beq mainbeq	; jump through another beq (-1 byte, +3 cycles)
#endif

	; EOF
eof
; Adding DLZ makes the routines 22 bytes longer
#if DLZ = 1
	lda LZPOS	;2 LZLEN
	cmp #2		;2
	beq 0$		;2 Really EOF
	jsr get8bit	;3 get ADD
	sta addi+1	;3
	tya		;1
	beq lz77_2	;2=22
0$
#endif

#if MACH = C64
	lda #$37	;+1ftMemConfig		** PARAMETER
	sta 1
	dec $d030	;+0ftFastDisable	** or "bit $d030" if 2MHz mode is not enabled
#endif
#if MACH = C128
	lda #$00
	sta $ff00
#endif
	lda OUTPOS	; Set the basic prg end address
	sta BASEND
	lda OUTPOS+1
	sta BASEND+1
#if MACH = C16
	; setting $ff06 bit 4, clear $f9 at end
	lda $ff06
	ora #$10
	sta $ff06	;+0ftFastDisable	** reveal the screen
	sta $ff3e	; ** could be a PARAMETER (switch on ROM/RAM)
	lda #0
	sta $f9
#endif
	cli		;+0ftCli		** PARAMETER
#if BASIC == 1
#if MACH == C64
    ;(0302) = a483
	;jsr $a533	; relink lines
#if 0
	lda #0	; set Z
	jsr $a871	; run + direct mode->program mode
#else
	jsr $a659
#endif
	jmp $a7ae	; basic warm start
#endif
#if MACH == VIC20
    	 ;(0302) = c483
	;jsr $c533	; relink lines
#if 0
	lda #0	; set Z
	jsr $c871	; run
#else
	jsr $c659
#endif
	jmp $c7ae	; basic warm start
#endif
#if MACH == C128
	jsr $51f3
	jsr $5a81
	jsr $4af6
	jmp ($0302)	;(0302) = 4dc6
;51F3
; $5254	; Back Up Text Pointer
; Perform [clr]
; $927B	; Call 'clall'
; Pointer: Limit-of-memory (bank 1)
; Pointer: Start-of-variables (bank 1)
; Pudef Characters
; $5AE1; Pointer: Start-of-BASIC (bank 0)
; Clear Stack & Work Area
;5a81 ;auto line number increment, collision mask, auto-insert mode flag..
;4af6 ??
;4dc6 basic warm start jmp ($0302)
#endif
#if MACH == C16
	;8a98 perform [clr]
	; z=1 ffe7 -> (032a) -> ef08 CLALL
	;     set basic pointers etc.
	jsr $8bbe	;perform run+clr
	jmp $8bea ;;$8bd6 ;jmp ($0302)	;(0302) = 8712
#endif
#else
	jmp $aaaa	;+1ftExecLo+2ftExecHi	** PARAMETER
#endif


table	dc.b 0,0,0,0,0,0,0	; the table must be at the end of the file!
	dc.b 0,0,0,0,0,0,0,0
	;dc.b 0,0,0,0,0,0,0,0
	;dc.b 0,0,0,0,0,0,0,0

#rend
block_stack_end


#endif

