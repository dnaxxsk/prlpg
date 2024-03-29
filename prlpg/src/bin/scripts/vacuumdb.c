/*-------------------------------------------------------------------------
 *
 * vacuumdb
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/scripts/vacuumdb.c,v 1.35 2010/02/17 04:19:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"


static void vacuum_one_database(const char *dbname, bool full, bool verbose,
					bool and_analyze, bool analyze_only, bool freeze,
					const char *table, const char *host, const char *port,
					const char *username, enum trivalue prompt_password,
					const char *progname, bool echo);
static void vacuum_all_databases(bool full, bool verbose, bool and_analyze,
					 bool analyze_only, bool freeze,
					 const char *host, const char *port,
					 const char *username, enum trivalue prompt_password,
					 const char *progname, bool echo, bool quiet);

static void help(const char *progname);


int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"echo", no_argument, NULL, 'e'},
		{"quiet", no_argument, NULL, 'q'},
		{"dbname", required_argument, NULL, 'd'},
		{"analyze", no_argument, NULL, 'z'},
		{"analyze-only", no_argument, NULL, 'Z'},
		{"freeze", no_argument, NULL, 'F'},
		{"all", no_argument, NULL, 'a'},
		{"table", required_argument, NULL, 't'},
		{"full", no_argument, NULL, 'f'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;

	const char *dbname = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	enum trivalue prompt_password = TRI_DEFAULT;
	bool		echo = false;
	bool		quiet = false;
	bool		and_analyze = false;
	bool		analyze_only = false;
	bool		freeze = false;
	bool		alldb = false;
	char	   *table = NULL;
	bool		full = false;
	bool		verbose = false;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "vacuumdb", help);

	while ((c = getopt_long(argc, argv, "h:p:U:wWeqd:zaFt:fv", long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'h':
				host = optarg;
				break;
			case 'p':
				port = optarg;
				break;
			case 'U':
				username = optarg;
				break;
			case 'w':
				prompt_password = TRI_NO;
				break;
			case 'W':
				prompt_password = TRI_YES;
				break;
			case 'e':
				echo = true;
				break;
			case 'q':
				quiet = true;
				break;
			case 'd':
				dbname = optarg;
				break;
			case 'z':
				and_analyze = true;
				break;
			case 'Z':
				analyze_only = true;
				break;
			case 'F':
				freeze = true;
				break;
			case 'a':
				alldb = true;
				break;
			case 't':
				table = optarg;
				break;
			case 'f':
				full = true;
				break;
			case 'v':
				verbose = true;
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	switch (argc - optind)
	{
		case 0:
			break;
		case 1:
			dbname = argv[optind];
			break;
		default:
			fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
					progname, argv[optind + 1]);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
			exit(1);
	}

	if (analyze_only)
	{
		if (full)
		{
			fprintf(stderr, _("%s: cannot use the \"full\" option when performing only analyze\n"),
					progname);
			exit(1);
		}
		if (freeze)
		{
			fprintf(stderr, _("%s: cannot use the \"freeze\" option when performing only analyze\n"),
					progname);
			exit(1);
		}
		/* allow 'and_analyze' with 'analyze_only' */
	}

	setup_cancel_handler();

	if (alldb)
	{
		if (dbname)
		{
			fprintf(stderr, _("%s: cannot vacuum all databases and a specific one at the same time\n"),
					progname);
			exit(1);
		}
		if (table)
		{
			fprintf(stderr, _("%s: cannot vacuum a specific table in all databases\n"),
					progname);
			exit(1);
		}

		vacuum_all_databases(full, verbose, and_analyze, analyze_only, freeze,
							 host, port, username, prompt_password,
							 progname, echo, quiet);
	}
	else
	{
		if (dbname == NULL)
		{
			if (getenv("PGDATABASE"))
				dbname = getenv("PGDATABASE");
			else if (getenv("PGUSER"))
				dbname = getenv("PGUSER");
			else
				dbname = get_user_name(progname);
		}

		vacuum_one_database(dbname, full, verbose, and_analyze, analyze_only,
							freeze, table,
							host, port, username, prompt_password,
							progname, echo);
	}

	exit(0);
}


static void
vacuum_one_database(const char *dbname, bool full, bool verbose, bool and_analyze,
					bool analyze_only, bool freeze, const char *table,
					const char *host, const char *port,
					const char *username, enum trivalue prompt_password,
					const char *progname, bool echo)
{
	PQExpBufferData sql;

	PGconn	   *conn;

	initPQExpBuffer(&sql);

	conn = connectDatabase(dbname, host, port, username, prompt_password, progname);

	if (analyze_only)
	{
		appendPQExpBuffer(&sql, "ANALYZE");
		if (verbose)
			appendPQExpBuffer(&sql, " VERBOSE");
	}
	else
	{
		appendPQExpBuffer(&sql, "VACUUM");
		if (PQserverVersion(conn) >= 90000)
		{
			const char *paren = " (";
			const char *comma = ", ";
			const char *sep = paren;

			if (full)
			{
				appendPQExpBuffer(&sql, "%sFULL", sep);
				sep = comma;
			}
			if (freeze)
			{
				appendPQExpBuffer(&sql, "%sFREEZE", sep);
				sep = comma;
			}
			if (verbose)
			{
				appendPQExpBuffer(&sql, "%sVERBOSE", sep);
				sep = comma;
			}
			if (and_analyze)
			{
				appendPQExpBuffer(&sql, "%sANALYZE", sep);
				sep = comma;
			}
			if (sep != paren)
				appendPQExpBuffer(&sql, ")");
		}
		else
		{
			if (full)
				appendPQExpBuffer(&sql, " FULL");
			if (freeze)
				appendPQExpBuffer(&sql, " FREEZE");
			if (verbose)
				appendPQExpBuffer(&sql, " VERBOSE");
			if (and_analyze)
				appendPQExpBuffer(&sql, " ANALYZE");
		}
	}
	if (table)
		appendPQExpBuffer(&sql, " %s", table);
	appendPQExpBuffer(&sql, ";\n");

	if (!executeMaintenanceCommand(conn, sql.data, echo))
	{
		if (table)
			fprintf(stderr, _("%s: vacuuming of table \"%s\" in database \"%s\" failed: %s"),
					progname, table, dbname, PQerrorMessage(conn));
		else
			fprintf(stderr, _("%s: vacuuming of database \"%s\" failed: %s"),
					progname, dbname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
	PQfinish(conn);
	termPQExpBuffer(&sql);
}


static void
vacuum_all_databases(bool full, bool verbose, bool and_analyze, bool analyze_only,
					 bool freeze, const char *host, const char *port,
					 const char *username, enum trivalue prompt_password,
					 const char *progname, bool echo, bool quiet)
{
	PGconn	   *conn;
	PGresult   *result;
	int			i;

	conn = connectDatabase("postgres", host, port, username, prompt_password, progname);
	result = executeQuery(conn, "SELECT datname FROM pg_database WHERE datallowconn ORDER BY 1;", progname, echo);
	PQfinish(conn);

	for (i = 0; i < PQntuples(result); i++)
	{
		char	   *dbname = PQgetvalue(result, i, 0);

		if (!quiet)
		{
			printf(_("%s: vacuuming database \"%s\"\n"), progname, dbname);
			fflush(stdout);
		}

		vacuum_one_database(dbname, full, verbose, and_analyze, analyze_only,
							freeze, NULL, host, port, username, prompt_password,
							progname, echo);
	}

	PQclear(result);
}


static void
help(const char *progname)
{
	printf(_("%s cleans and analyzes a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -a, --all                       vacuum all databases\n"));
	printf(_("  -d, --dbname=DBNAME             database to vacuum\n"));
	printf(_("  -e, --echo                      show the commands being sent to the server\n"));
	printf(_("  -f, --full                      do full vacuuming\n"));
	printf(_("  -F, --freeze                    freeze row transaction information\n"));
	printf(_("  -q, --quiet                     don't write any messages\n"));
	printf(_("  -t, --table='TABLE[(COLUMNS)]'  vacuum specific table only\n"));
	printf(_("  -v, --verbose                   write a lot of output\n"));
	printf(_("  -z, --analyze                   update optimizer hints\n"));
	printf(_("  -Z, --analyze-only              only update optimizer hints\n"));
	printf(_("  --help                          show this help, then exit\n"));
	printf(_("  --version                       output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -w, --no-password         never prompt for password\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("\nRead the description of the SQL command VACUUM for details.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
