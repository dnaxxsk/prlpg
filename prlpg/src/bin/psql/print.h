/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2010, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/print.h,v 1.43 2010/01/02 16:57:59 momjian Exp $
 */
#ifndef PRINT_H
#define PRINT_H

#include "libpq-fe.h"


enum printFormat
{
	PRINT_NOTHING = 0,			/* to make sure someone initializes this */
	PRINT_UNALIGNED,
	PRINT_ALIGNED,
	PRINT_WRAPPED,
	PRINT_HTML,
	PRINT_LATEX,
	PRINT_TROFF_MS
	/* add your favourite output format here ... */
};

typedef struct printTextLineFormat
{
	/* Line drawing characters to be used in various contexts */
	const char *hrule;			/* horizontal line character */
	const char *leftvrule;		/* left vertical line (+horizontal) */
	const char *midvrule;		/* intra-column vertical line (+horizontal) */
	const char *rightvrule;		/* right vertical line (+horizontal) */
} printTextLineFormat;

typedef enum printTextRule
{
	/* Additional context for selecting line drawing characters */
	PRINT_RULE_TOP,				/* top horizontal line */
	PRINT_RULE_MIDDLE,			/* intra-data horizontal line */
	PRINT_RULE_BOTTOM,			/* bottom horizontal line */
	PRINT_RULE_DATA				/* data line (hrule is unused here) */
} printTextRule;

typedef enum printTextLineWrap
{
	/* Line wrapping conditions */
	PRINT_LINE_WRAP_NONE,		/* No wrapping */
	PRINT_LINE_WRAP_WRAP,		/* Wraparound due to overlength line */
	PRINT_LINE_WRAP_NEWLINE		/* Newline in data */
} printTextLineWrap;

typedef struct printTextFormat
{
	/* A complete line style */
	const char *name;				/* for display purposes */
	printTextLineFormat lrule[4];	/* indexed by enum printTextRule */
	const char *midvrule_nl;	/* vertical line for continue after newline */
	const char *midvrule_wrap;	/* vertical line for wrapped data */
	const char *midvrule_blank;	/* vertical line for blank data */
	const char *header_nl_left;	/* left mark after newline */
	const char *header_nl_right; /* right mark for newline */
	const char *nl_left;		/* left mark after newline */
	const char *nl_right;		/* right mark for newline */
	const char *wrap_left;		/* left mark after wrapped data */
	const char *wrap_right;		/* right mark for wrapped data */
	bool		wrap_right_border;	/* use right-hand border for wrap marks
									 * when border=0? */
} printTextFormat;

typedef struct printTableOpt
{
	enum printFormat format;	/* see enum above */
	bool		expanded;		/* expanded/vertical output (if supported by
								 * output format) */
	unsigned short int border;	/* Print a border around the table. 0=none,
								 * 1=dividing lines, 2=full */
	unsigned short int pager;	/* use pager for output (if to stdout and
								 * stdout is a tty) 0=off 1=on 2=always */
	bool		tuples_only;	/* don't output headers, row counts, etc. */
	bool		start_table;	/* print start decoration, eg <table> */
	bool		stop_table;		/* print stop decoration, eg </table> */
	unsigned long prior_records;	/* start offset for record counters */
	const printTextFormat *line_style;	/* line style (NULL for default) */
	char	   *fieldSep;		/* field separator for unaligned text mode */
	char	   *recordSep;		/* record separator for unaligned text mode */
	bool		numericLocale;	/* locale-aware numeric units separator and
								 * decimal marker */
	char	   *tableAttr;		/* attributes for HTML <table ...> */
	int			encoding;		/* character encoding */
	int			env_columns;	/* $COLUMNS on psql start, 0 is unset */
	int			columns;		/* target width for wrapped format */
} printTableOpt;

/*
 * Table footers are implemented as a singly-linked list.
 *
 * This is so that you don't need to know the number of footers in order to
 * initialise the printTableContent struct, which is very convenient when
 * preparing complex footers (as in describeOneTableDetails).
 */
typedef struct printTableFooter
{
	char	   *data;
	struct printTableFooter *next;
} printTableFooter;

/*
 * The table content struct holds all the information which will be displayed
 * by printTable().
 */
typedef struct printTableContent
{
	const printTableOpt *opt;
	const char *title;			/* May be NULL */
	int			ncolumns;		/* Specified in Init() */
	int			nrows;			/* Specified in Init() */
	const char **headers;		/* NULL-terminated array of header strings */
	const char **header;		/* Pointer to the last added header */
	const char **cells;			/* NULL-terminated array of cell content
								 * strings */
	const char **cell;			/* Pointer to the last added cell */
	printTableFooter *footers;	/* Pointer to the first footer */
	printTableFooter *footer;	/* Pointer to the last added footer */
	char	   *aligns;			/* Array of alignment specifiers; 'l' or 'r',
								 * one per column */
	char	   *align;			/* Pointer to the last added alignment */
} printTableContent;

typedef struct printQueryOpt
{
	printTableOpt topt;			/* the options above */
	char	   *nullPrint;		/* how to print null entities */
	bool		quote;			/* quote all values as much as possible */
	char	   *title;			/* override title */
	char	  **footers;		/* override footer (default is "(xx rows)") */
	bool		default_footer; /* print default footer if footers==NULL */
	bool		translate_header;		/* do gettext on column headers */
	const bool *translate_columns;		/* translate_columns[i-1] => do
										 * gettext on col i */
} printQueryOpt;


extern const printTextFormat pg_asciiformat;
extern const printTextFormat pg_asciiformat_old;
extern const printTextFormat pg_utf8format;


extern FILE *PageOutput(int lines, unsigned short int pager);
extern void ClosePager(FILE *pagerpipe);

extern void html_escaped_print(const char *in, FILE *fout);

extern void printTableInit(printTableContent *const content,
			   const printTableOpt *opt, const char *title,
			   const int ncolumns, const int nrows);
extern void printTableAddHeader(printTableContent *const content,
				 const char *header, const bool translate, const char align);
extern void printTableAddCell(printTableContent *const content,
				  const char *cell, const bool translate);
extern void printTableAddFooter(printTableContent *const content,
					const char *footer);
extern void printTableSetFooter(printTableContent *const content,
					const char *footer);
extern void printTableCleanup(printTableContent *const content);
extern void printTable(const printTableContent *cont, FILE *fout, FILE *flog);
extern void printQuery(const PGresult *result, const printQueryOpt *opt,
		   FILE *fout, FILE *flog);

extern void setDecimalLocale(void);
extern const printTextFormat *get_line_style(const printTableOpt *opt);

#ifndef __CYGWIN__
#define DEFAULT_PAGER "more"
#else
#define DEFAULT_PAGER "less"
#endif

#endif   /* PRINT_H */
