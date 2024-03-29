/*
 * This file contains public functions for conversion between
 * client encoding and server (database) encoding.
 *
 * Tatsuo Ishii
 *
 * $PostgreSQL: pgsql/src/backend/utils/mb/mbutils.c,v 1.93 2010/02/14 18:42:18 rhaas Exp $
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

/*
 * When converting strings between different encodings, we assume that space
 * for converted result is 4-to-1 growth in the worst case. The rate for
 * currently supported encoding pairs are within 3 (SJIS JIS X0201 half width
 * kanna -> UTF8 is the worst case).  So "4" should be enough for the moment.
 *
 * Note that this is not the same as the maximum character width in any
 * particular encoding.
 */
#define MAX_CONVERSION_GROWTH  4

/*
 * We maintain a simple linked list caching the fmgr lookup info for the
 * currently selected conversion functions, as well as any that have been
 * selected previously in the current session.	(We remember previous
 * settings because we must be able to restore a previous setting during
 * transaction rollback, without doing any fresh catalog accesses.)
 *
 * Since we'll never release this data, we just keep it in TopMemoryContext.
 */
typedef struct ConvProcInfo
{
	int			s_encoding;		/* server and client encoding IDs */
	int			c_encoding;
	FmgrInfo	to_server_info; /* lookup info for conversion procs */
	FmgrInfo	to_client_info;
} ConvProcInfo;

static List *ConvProcList = NIL;	/* List of ConvProcInfo */

/*
 * These variables point to the currently active conversion functions,
 * or are NULL when no conversion is needed.
 */
static FmgrInfo *ToServerConvProc = NULL;
static FmgrInfo *ToClientConvProc = NULL;

/*
 * These variables track the currently selected FE and BE encodings.
 */
static pg_enc2name *ClientEncoding = &pg_enc2name_tbl[PG_SQL_ASCII];
static pg_enc2name *DatabaseEncoding = &pg_enc2name_tbl[PG_SQL_ASCII];
static pg_enc2name *PlatformEncoding = NULL;

/*
 * During backend startup we can't set client encoding because we (a)
 * can't look up the conversion functions, and (b) may not know the database
 * encoding yet either.  So SetClientEncoding() just accepts anything and
 * remembers it for InitializeClientEncoding() to apply later.
 */
static bool backend_startup_complete = false;
static int	pending_client_encoding = PG_SQL_ASCII;


/* Internal functions */
static char *perform_default_encoding_conversion(const char *src,
									int len, bool is_client_to_server);
static int	cliplen(const char *str, int len, int limit);


/*
 * Set the client encoding and save fmgrinfo for the conversion
 * function if necessary.  Returns 0 if okay, -1 if not (bad encoding
 * or can't support conversion)
 */
int
SetClientEncoding(int encoding, bool doit)
{
	int			current_server_encoding;
	ListCell   *lc;

	if (!PG_VALID_FE_ENCODING(encoding))
		return -1;

	/* Can't do anything during startup, per notes above */
	if (!backend_startup_complete)
	{
		if (doit)
			pending_client_encoding = encoding;
		return 0;
	}

	current_server_encoding = GetDatabaseEncoding();

	/*
	 * Check for cases that require no conversion function.
	 */
	if (current_server_encoding == encoding ||
		current_server_encoding == PG_SQL_ASCII ||
		encoding == PG_SQL_ASCII)
	{
		if (doit)
		{
			ClientEncoding = &pg_enc2name_tbl[encoding];
			ToServerConvProc = NULL;
			ToClientConvProc = NULL;
		}
		return 0;
	}

	if (IsTransactionState())
	{
		/*
		 * If we're in a live transaction, it's safe to access the catalogs,
		 * so look up the functions.  We repeat the lookup even if the info is
		 * already cached, so that we can react to changes in the contents of
		 * pg_conversion.
		 */
		Oid			to_server_proc,
					to_client_proc;
		ConvProcInfo *convinfo;
		MemoryContext oldcontext;

		to_server_proc = FindDefaultConversionProc(encoding,
												   current_server_encoding);
		if (!OidIsValid(to_server_proc))
			return -1;
		to_client_proc = FindDefaultConversionProc(current_server_encoding,
												   encoding);
		if (!OidIsValid(to_client_proc))
			return -1;

		/*
		 * Done if not wanting to actually apply setting.
		 */
		if (!doit)
			return 0;

		/*
		 * Load the fmgr info into TopMemoryContext (could still fail here)
		 */
		convinfo = (ConvProcInfo *) MemoryContextAlloc(TopMemoryContext,
													   sizeof(ConvProcInfo));
		convinfo->s_encoding = current_server_encoding;
		convinfo->c_encoding = encoding;
		fmgr_info_cxt(to_server_proc, &convinfo->to_server_info,
					  TopMemoryContext);
		fmgr_info_cxt(to_client_proc, &convinfo->to_client_info,
					  TopMemoryContext);

		/* Attach new info to head of list */
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		ConvProcList = lcons(convinfo, ConvProcList);
		MemoryContextSwitchTo(oldcontext);

		/*
		 * Everything is okay, so apply the setting.
		 */
		ClientEncoding = &pg_enc2name_tbl[encoding];
		ToServerConvProc = &convinfo->to_server_info;
		ToClientConvProc = &convinfo->to_client_info;

		/*
		 * Remove any older entry for the same encoding pair (this is just to
		 * avoid memory leakage).
		 */
		foreach(lc, ConvProcList)
		{
			ConvProcInfo *oldinfo = (ConvProcInfo *) lfirst(lc);

			if (oldinfo == convinfo)
				continue;
			if (oldinfo->s_encoding == convinfo->s_encoding &&
				oldinfo->c_encoding == convinfo->c_encoding)
			{
				ConvProcList = list_delete_ptr(ConvProcList, oldinfo);
				pfree(oldinfo);
				break;			/* need not look further */
			}
		}

		return 0;				/* success */
	}
	else
	{
		/*
		 * If we're not in a live transaction, the only thing we can do is
		 * restore a previous setting using the cache.	This covers all
		 * transaction-rollback cases.	The only case it might not work for is
		 * trying to change client_encoding on the fly by editing
		 * postgresql.conf and SIGHUP'ing.  Which would probably be a stupid
		 * thing to do anyway.
		 */
		foreach(lc, ConvProcList)
		{
			ConvProcInfo *oldinfo = (ConvProcInfo *) lfirst(lc);

			if (oldinfo->s_encoding == current_server_encoding &&
				oldinfo->c_encoding == encoding)
			{
				if (doit)
				{
					ClientEncoding = &pg_enc2name_tbl[encoding];
					ToServerConvProc = &oldinfo->to_server_info;
					ToClientConvProc = &oldinfo->to_client_info;
				}
				return 0;
			}
		}

		return -1;				/* it's not cached, so fail */
	}
}

/*
 * Initialize client encoding if necessary.
 *		called from InitPostgres() once during backend startup.
 */
void
InitializeClientEncoding(void)
{
	Assert(!backend_startup_complete);
	backend_startup_complete = true;

	if (SetClientEncoding(pending_client_encoding, true) < 0)
	{
		/*
		 * Oops, the requested conversion is not available. We couldn't fail
		 * before, but we can now.
		 */
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("conversion between %s and %s is not supported",
						pg_enc2name_tbl[pending_client_encoding].name,
						GetDatabaseEncodingName())));
	}
}

/*
 * returns the current client encoding
 */
int
pg_get_client_encoding(void)
{
	Assert(ClientEncoding);
	return ClientEncoding->encoding;
}

/*
 * returns the current client encoding name
 */
const char *
pg_get_client_encoding_name(void)
{
	Assert(ClientEncoding);
	return ClientEncoding->name;
}

/*
 * Apply encoding conversion on src and return it. The encoding
 * conversion function is chosen from the pg_conversion system catalog
 * marked as "default". If it is not found in the schema search path,
 * it's taken from pg_catalog schema. If it even is not in the schema,
 * warn and return src.
 *
 * If conversion occurs, a palloc'd null-terminated string is returned.
 * In the case of no conversion, src is returned.
 *
 * CAUTION: although the presence of a length argument means that callers
 * can pass non-null-terminated strings, care is required because the same
 * string will be passed back if no conversion occurs.	Such callers *must*
 * check whether result == src and handle that case differently.
 *
 * Note: we try to avoid raising error, since that could get us into
 * infinite recursion when this function is invoked during error message
 * sending.  It should be OK to raise error for overlength strings though,
 * since the recursion will come with a shorter message.
 */
unsigned char *
pg_do_encoding_conversion(unsigned char *src, int len,
						  int src_encoding, int dest_encoding)
{
	unsigned char *result;
	Oid			proc;

	if (!IsTransactionState())
		return src;

	if (src_encoding == dest_encoding)
		return src;

	if (src_encoding == PG_SQL_ASCII || dest_encoding == PG_SQL_ASCII)
		return src;

	if (len <= 0)
		return src;

	proc = FindDefaultConversionProc(src_encoding, dest_encoding);
	if (!OidIsValid(proc))
	{
		ereport(LOG,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("default conversion function for encoding \"%s\" to \"%s\" does not exist",
						pg_encoding_to_char(src_encoding),
						pg_encoding_to_char(dest_encoding))));
		return src;
	}

	/*
	 * XXX we should avoid throwing errors in OidFunctionCall. Otherwise we
	 * are going into infinite loop!  So we have to make sure that the
	 * function exists before calling OidFunctionCall.
	 */
	if (!SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(proc)))
	{
		elog(LOG, "cache lookup failed for function %u", proc);
		return src;
	}

	/*
	 * Allocate space for conversion result, being wary of integer overflow
	 */
	if ((Size) len >= (MaxAllocSize / (Size) MAX_CONVERSION_GROWTH))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory"),
		 errdetail("String of %d bytes is too long for encoding conversion.",
				   len)));

	result = palloc(len * MAX_CONVERSION_GROWTH + 1);

	OidFunctionCall5(proc,
					 Int32GetDatum(src_encoding),
					 Int32GetDatum(dest_encoding),
					 CStringGetDatum(src),
					 CStringGetDatum(result),
					 Int32GetDatum(len));
	return result;
}

/*
 * Convert string using encoding_name. The source
 * encoding is the DB encoding.
 *
 * BYTEA convert_to(TEXT string, NAME encoding_name) */
Datum
pg_convert_to(PG_FUNCTION_ARGS)
{
	Datum		string = PG_GETARG_DATUM(0);
	Datum		dest_encoding_name = PG_GETARG_DATUM(1);
	Datum		src_encoding_name = DirectFunctionCall1(namein,
									CStringGetDatum(DatabaseEncoding->name));
	Datum		result;

	/*
	 * pg_convert expects a bytea as its first argument. We're passing it a
	 * text argument here, relying on the fact that they are both in fact
	 * varlena types, and thus structurally identical.
	 */
	result = DirectFunctionCall3(pg_convert, string,
								 src_encoding_name, dest_encoding_name);

	PG_RETURN_DATUM(result);
}

/*
 * Convert string using encoding_name. The destination
 * encoding is the DB encoding.
 *
 * TEXT convert_from(BYTEA string, NAME encoding_name) */
Datum
pg_convert_from(PG_FUNCTION_ARGS)
{
	Datum		string = PG_GETARG_DATUM(0);
	Datum		src_encoding_name = PG_GETARG_DATUM(1);
	Datum		dest_encoding_name = DirectFunctionCall1(namein,
									CStringGetDatum(DatabaseEncoding->name));
	Datum		result;

	result = DirectFunctionCall3(pg_convert, string,
								 src_encoding_name, dest_encoding_name);

	/*
	 * pg_convert returns a bytea, which we in turn return as text, relying on
	 * the fact that they are both in fact varlena types, and thus
	 * structurally identical. Although not all bytea values are valid text,
	 * in this case it will be because we've told pg_convert to return one
	 * that is valid as text in the current database encoding.
	 */
	PG_RETURN_DATUM(result);
}

/*
 * Convert string using encoding_names.
 *
 * BYTEA convert(BYTEA string, NAME src_encoding_name, NAME dest_encoding_name)
 */
Datum
pg_convert(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_P(0);
	char	   *src_encoding_name = NameStr(*PG_GETARG_NAME(1));
	int			src_encoding = pg_char_to_encoding(src_encoding_name);
	char	   *dest_encoding_name = NameStr(*PG_GETARG_NAME(2));
	int			dest_encoding = pg_char_to_encoding(dest_encoding_name);
	unsigned char *result;
	bytea	   *retval;
	unsigned char *str;
	int			len;

	if (src_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid source encoding name \"%s\"",
						src_encoding_name)));
	if (dest_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid destination encoding name \"%s\"",
						dest_encoding_name)));

	/* make sure that source string is valid and null terminated */
	len = VARSIZE(string) - VARHDRSZ;
	pg_verify_mbstr(src_encoding, VARDATA(string), len, false);
	str = palloc(len + 1);
	memcpy(str, VARDATA(string), len);
	*(str + len) = '\0';

	result = pg_do_encoding_conversion(str, len, src_encoding, dest_encoding);

	/*
	 * build bytea data type structure.
	 */
	len = strlen((char *) result) + VARHDRSZ;
	retval = palloc(len);
	SET_VARSIZE(retval, len);
	memcpy(VARDATA(retval), result, len - VARHDRSZ);

	if (result != str)
		pfree(result);
	pfree(str);

	/* free memory if allocated by the toaster */
	PG_FREE_IF_COPY(string, 0);

	PG_RETURN_BYTEA_P(retval);
}

/*
 * get the length of the string considered as text in the specified
 * encoding. Raises an error if the data is not valid in that
 * encoding.
 *
 * INT4 length (BYTEA string, NAME src_encoding_name)
 */
Datum
length_in_encoding(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_P(0);
	char	   *src_encoding_name = NameStr(*PG_GETARG_NAME(1));
	int			src_encoding = pg_char_to_encoding(src_encoding_name);
	int			len = VARSIZE(string) - VARHDRSZ;
	int			retval;

	if (src_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding name \"%s\"",
						src_encoding_name)));

	retval = pg_verify_mbstr_len(src_encoding, VARDATA(string), len, false);
	PG_RETURN_INT32(retval);

}

Datum
pg_encoding_max_length_sql(PG_FUNCTION_ARGS)
{
	int encoding = PG_GETARG_INT32(0);

	if (PG_VALID_ENCODING(encoding))
		PG_RETURN_INT32(pg_wchar_table[encoding].maxmblen);
	else
		PG_RETURN_NULL();
}

/*
 * convert client encoding to server encoding.
 */
char *
pg_client_to_server(const char *s, int len)
{
	Assert(DatabaseEncoding);
	Assert(ClientEncoding);

	if (len <= 0)
		return (char *) s;

	if (ClientEncoding->encoding == DatabaseEncoding->encoding ||
		ClientEncoding->encoding == PG_SQL_ASCII)
	{
		/*
		 * No conversion is needed, but we must still validate the data.
		 */
		(void) pg_verify_mbstr(DatabaseEncoding->encoding, s, len, false);
		return (char *) s;
	}

	if (DatabaseEncoding->encoding == PG_SQL_ASCII)
	{
		/*
		 * No conversion is possible, but we must still validate the data,
		 * because the client-side code might have done string escaping using
		 * the selected client_encoding.  If the client encoding is ASCII-safe
		 * then we just do a straight validation under that encoding.  For an
		 * ASCII-unsafe encoding we have a problem: we dare not pass such data
		 * to the parser but we have no way to convert it.	We compromise by
		 * rejecting the data if it contains any non-ASCII characters.
		 */
		if (PG_VALID_BE_ENCODING(ClientEncoding->encoding))
			(void) pg_verify_mbstr(ClientEncoding->encoding, s, len, false);
		else
		{
			int			i;

			for (i = 0; i < len; i++)
			{
				if (s[i] == '\0' || IS_HIGHBIT_SET(s[i]))
					ereport(ERROR,
							(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
					 errmsg("invalid byte value for encoding \"%s\": 0x%02x",
							pg_enc2name_tbl[PG_SQL_ASCII].name,
							(unsigned char) s[i])));
			}
		}
		return (char *) s;
	}

	return perform_default_encoding_conversion(s, len, true);
}

/*
 * convert server encoding to client encoding.
 */
char *
pg_server_to_client(const char *s, int len)
{
	Assert(DatabaseEncoding);
	Assert(ClientEncoding);

	if (len <= 0)
		return (char *) s;

	if (ClientEncoding->encoding == DatabaseEncoding->encoding ||
		ClientEncoding->encoding == PG_SQL_ASCII ||
		DatabaseEncoding->encoding == PG_SQL_ASCII)
		return (char *) s;		/* assume data is valid */

	return perform_default_encoding_conversion(s, len, false);
}

/*
 *	Perform default encoding conversion using cached FmgrInfo. Since
 *	this function does not access database at all, it is safe to call
 *	outside transactions.  If the conversion has not been set up by
 *	SetClientEncoding(), no conversion is performed.
 */
static char *
perform_default_encoding_conversion(const char *src, int len, bool is_client_to_server)
{
	char	   *result;
	int			src_encoding,
				dest_encoding;
	FmgrInfo   *flinfo;

	if (is_client_to_server)
	{
		src_encoding = ClientEncoding->encoding;
		dest_encoding = DatabaseEncoding->encoding;
		flinfo = ToServerConvProc;
	}
	else
	{
		src_encoding = DatabaseEncoding->encoding;
		dest_encoding = ClientEncoding->encoding;
		flinfo = ToClientConvProc;
	}

	if (flinfo == NULL)
		return (char *) src;

	/*
	 * Allocate space for conversion result, being wary of integer overflow
	 */
	if ((Size) len >= (MaxAllocSize / (Size) MAX_CONVERSION_GROWTH))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory"),
		 errdetail("String of %d bytes is too long for encoding conversion.",
				   len)));

	result = palloc(len * MAX_CONVERSION_GROWTH + 1);

	FunctionCall5(flinfo,
				  Int32GetDatum(src_encoding),
				  Int32GetDatum(dest_encoding),
				  CStringGetDatum(src),
				  CStringGetDatum(result),
				  Int32GetDatum(len));
	return result;
}



#ifdef USE_WIDE_UPPER_LOWER

/*
 * wchar2char --- convert wide characters to multibyte format
 *
 * This has the same API as the standard wcstombs() function; in particular,
 * tolen is the maximum number of bytes to store at *to, and *from must be
 * zero-terminated.  The output will be zero-terminated iff there is room.
 */
size_t
wchar2char(char *to, const wchar_t *from, size_t tolen)
{
	size_t		result;

	if (tolen == 0)
		return 0;

#ifdef WIN32

	/*
	 * On Windows, the "Unicode" locales assume UTF16 not UTF8 encoding, and
	 * for some reason mbstowcs and wcstombs won't do this for us, so we use
	 * MultiByteToWideChar().
	 */
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		result = WideCharToMultiByte(CP_UTF8, 0, from, -1, to, tolen,
									 NULL, NULL);
		/* A zero return is failure */
		if (result <= 0)
			result = -1;
		else
		{
			Assert(result <= tolen);
			/* Microsoft counts the zero terminator in the result */
			result--;
		}
	}
	else
#endif   /* WIN32 */
	{
		Assert(!lc_ctype_is_c());
		result = wcstombs(to, from, tolen);
	}
	return result;
}

/*
 * char2wchar --- convert multibyte characters to wide characters
 *
 * This has almost the API of mbstowcs(), except that *from need not be
 * null-terminated; instead, the number of input bytes is specified as
 * fromlen.  Also, we ereport() rather than returning -1 for invalid
 * input encoding.	tolen is the maximum number of wchar_t's to store at *to.
 * The output will be zero-terminated iff there is room.
 */
size_t
char2wchar(wchar_t *to, size_t tolen, const char *from, size_t fromlen)
{
	size_t		result;

	if (tolen == 0)
		return 0;

#ifdef WIN32
	/* See WIN32 "Unicode" comment above */
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		/* Win32 API does not work for zero-length input */
		if (fromlen == 0)
			result = 0;
		else
		{
			result = MultiByteToWideChar(CP_UTF8, 0, from, fromlen, to, tolen - 1);
			/* A zero return is failure */
			if (result == 0)
				result = -1;
		}

		if (result != -1)
		{
			Assert(result < tolen);
			/* Append trailing null wchar (MultiByteToWideChar() does not) */
			to[result] = 0;
		}
	}
	else
#endif   /* WIN32 */
	{
		/* mbstowcs requires ending '\0' */
		char	   *str = pnstrdup(from, fromlen);

		Assert(!lc_ctype_is_c());
		result = mbstowcs(to, str, tolen);
		pfree(str);
	}

	if (result == -1)
	{
		/*
		 * Invalid multibyte character encountered.  We try to give a useful
		 * error message by letting pg_verifymbstr check the string.  But it's
		 * possible that the string is OK to us, and not OK to mbstowcs ---
		 * this suggests that the LC_CTYPE locale is different from the
		 * database encoding.  Give a generic error message if verifymbstr
		 * can't find anything wrong.
		 */
		pg_verifymbstr(from, fromlen, false);	/* might not return */
		/* but if it does ... */
		ereport(ERROR,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("invalid multibyte character for locale"),
				 errhint("The server's LC_CTYPE locale is probably incompatible with the database encoding.")));
	}

	return result;
}
#endif

/* convert a multibyte string to a wchar */
int
pg_mb2wchar(const char *from, pg_wchar *to)
{
	return (*pg_wchar_table[DatabaseEncoding->encoding].mb2wchar_with_len) ((const unsigned char *) from, to, strlen(from));
}

/* convert a multibyte string to a wchar with a limited length */
int
pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len)
{
	return (*pg_wchar_table[DatabaseEncoding->encoding].mb2wchar_with_len) ((const unsigned char *) from, to, len);
}

/* same, with any encoding */
int
pg_encoding_mb2wchar_with_len(int encoding,
							  const char *from, pg_wchar *to, int len)
{
	return (*pg_wchar_table[encoding].mb2wchar_with_len) ((const unsigned char *) from, to, len);
}

/* returns the byte length of a multibyte character */
int
pg_mblen(const char *mbstr)
{
	return ((*pg_wchar_table[DatabaseEncoding->encoding].mblen) ((const unsigned char *) mbstr));
}

/* returns the display length of a multibyte character */
int
pg_dsplen(const char *mbstr)
{
	return ((*pg_wchar_table[DatabaseEncoding->encoding].dsplen) ((const unsigned char *) mbstr));
}

/* returns the length (counted in wchars) of a multibyte string */
int
pg_mbstrlen(const char *mbstr)
{
	int			len = 0;

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() == 1)
		return strlen(mbstr);

	while (*mbstr)
	{
		mbstr += pg_mblen(mbstr);
		len++;
	}
	return len;
}

/* returns the length (counted in wchars) of a multibyte string
 * (not necessarily NULL terminated)
 */
int
pg_mbstrlen_with_len(const char *mbstr, int limit)
{
	int			len = 0;

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() == 1)
		return limit;

	while (limit > 0 && *mbstr)
	{
		int			l = pg_mblen(mbstr);

		limit -= l;
		mbstr += l;
		len++;
	}
	return len;
}

/*
 * returns the byte length of a multibyte string
 * (not necessarily NULL terminated)
 * that is no longer than limit.
 * this function does not break multibyte character boundary.
 */
int
pg_mbcliplen(const char *mbstr, int len, int limit)
{
	return pg_encoding_mbcliplen(DatabaseEncoding->encoding, mbstr,
								 len, limit);
}

/*
 * pg_mbcliplen with specified encoding
 */
int
pg_encoding_mbcliplen(int encoding, const char *mbstr,
					  int len, int limit)
{
	mblen_converter mblen_fn;
	int			clen = 0;
	int			l;

	/* optimization for single byte encoding */
	if (pg_encoding_max_length(encoding) == 1)
		return cliplen(mbstr, len, limit);

	mblen_fn = pg_wchar_table[encoding].mblen;

	while (len > 0 && *mbstr)
	{
		l = (*mblen_fn) ((const unsigned char *) mbstr);
		if ((clen + l) > limit)
			break;
		clen += l;
		if (clen == limit)
			break;
		len -= l;
		mbstr += l;
	}
	return clen;
}

/*
 * Similar to pg_mbcliplen except the limit parameter specifies the
 * character length, not the byte length.
 */
int
pg_mbcharcliplen(const char *mbstr, int len, int limit)
{
	int			clen = 0;
	int			nch = 0;
	int			l;

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() == 1)
		return cliplen(mbstr, len, limit);

	while (len > 0 && *mbstr)
	{
		l = pg_mblen(mbstr);
		nch++;
		if (nch > limit)
			break;
		clen += l;
		len -= l;
		mbstr += l;
	}
	return clen;
}

/* mbcliplen for any single-byte encoding */
static int
cliplen(const char *str, int len, int limit)
{
	int			l = 0;

	len = Min(len, limit);
	while (l < len && str[l])
		l++;
	return l;
}

void
SetDatabaseEncoding(int encoding)
{
	if (!PG_VALID_BE_ENCODING(encoding))
		elog(ERROR, "invalid database encoding: %d", encoding);

	DatabaseEncoding = &pg_enc2name_tbl[encoding];
	Assert(DatabaseEncoding->encoding == encoding);
}

/*
 * Bind gettext to the codeset equivalent with the database encoding.
 */
void
pg_bind_textdomain_codeset(const char *domainname)
{
#if defined(ENABLE_NLS)
	int			encoding = GetDatabaseEncoding();
	int			i;

	/*
	 * gettext() uses the codeset specified by LC_CTYPE by default, so if that
	 * matches the database encoding we don't need to do anything. In CREATE
	 * DATABASE, we enforce or trust that the locale's codeset matches
	 * database encoding, except for the C locale. In C locale, we bind
	 * gettext() explicitly to the right codeset.
	 *
	 * On Windows, though, gettext() tends to get confused so we always bind
	 * it.
	 */
#ifndef WIN32
	const char *ctype = setlocale(LC_CTYPE, NULL);

	if (pg_strcasecmp(ctype, "C") != 0 && pg_strcasecmp(ctype, "POSIX") != 0)
		return;
#endif

	for (i = 0; pg_enc2gettext_tbl[i].name != NULL; i++)
	{
		if (pg_enc2gettext_tbl[i].encoding == encoding)
		{
			if (bind_textdomain_codeset(domainname,
										pg_enc2gettext_tbl[i].name) == NULL)
				elog(LOG, "bind_textdomain_codeset failed");
			break;
		}
	}
#endif
}

int
GetDatabaseEncoding(void)
{
	Assert(DatabaseEncoding);
	return DatabaseEncoding->encoding;
}

const char *
GetDatabaseEncodingName(void)
{
	Assert(DatabaseEncoding);
	return DatabaseEncoding->name;
}

Datum
getdatabaseencoding(PG_FUNCTION_ARGS)
{
	Assert(DatabaseEncoding);
	return DirectFunctionCall1(namein, CStringGetDatum(DatabaseEncoding->name));
}

Datum
pg_client_encoding(PG_FUNCTION_ARGS)
{
	Assert(ClientEncoding);
	return DirectFunctionCall1(namein, CStringGetDatum(ClientEncoding->name));
}

int
GetPlatformEncoding(void)
{
	if (PlatformEncoding == NULL)
	{
		/* try to determine encoding of server's environment locale */
		int		encoding = pg_get_encoding_from_locale("");

		if (encoding < 0)
			encoding = PG_SQL_ASCII;
		PlatformEncoding = &pg_enc2name_tbl[encoding];
	}
	return PlatformEncoding->encoding;
}

#ifdef WIN32

/*
 * Result is palloc'ed null-terminated utf16 string. The character length
 * is also passed to utf16len if not null. Returns NULL iff failed.
 */
WCHAR *
pgwin32_toUTF16(const char *str, int len, int *utf16len)
{
	WCHAR	   *utf16;
	int			dstlen;
	UINT		codepage;

	codepage = pg_enc2name_tbl[GetDatabaseEncoding()].codepage;

	/*
	 * Use MultiByteToWideChar directly if there is a corresponding codepage,
	 * or double conversion through UTF8 if not.
	 */
	if (codepage != 0)
	{
		utf16 = (WCHAR *) palloc(sizeof(WCHAR) * (len + 1));
		dstlen = MultiByteToWideChar(codepage, 0, str, len, utf16, len);
		utf16[dstlen] = L'\0';
	}
	else
	{
		char	   *utf8;

		utf8 = (char *) pg_do_encoding_conversion((unsigned char *) str,
										len, GetDatabaseEncoding(), PG_UTF8);
		if (utf8 != str)
			len = strlen(utf8);

		utf16 = (WCHAR *) palloc(sizeof(WCHAR) * (len + 1));
		dstlen = MultiByteToWideChar(CP_UTF8, 0, utf8, len, utf16, len);
		utf16[dstlen] = L'\0';

		if (utf8 != str)
			pfree(utf8);
	}

	if (dstlen == 0 && len > 0)
	{
		pfree(utf16);
		return NULL;	/* error */
	}

	if (utf16len)
		*utf16len = dstlen;
	return utf16;
}

#endif
