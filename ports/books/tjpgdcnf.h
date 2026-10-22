/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/* The Libri app's choice (fetch.sh copies it over the archive's own): RGB888 output, descaling on
/  (1/2, 1/4, 1/8 while decoding: a comic page is never decoded whole), the fastest decoder (32-bit,
/  table-driven Huffman: 6 KB of the pool). */
/*----------------------------------------------*/

#define	JD_SZBUF		4096
#define JD_FORMAT		0
#define	JD_USE_SCALE	1
#define JD_TBLCLIP		1
#define JD_FASTDECODE	2
