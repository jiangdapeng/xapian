/* api_replicate.cc: tests of replication functionality
 *
 * Copyright 2008 Lemur Consulting Ltd
 * Copyright 2009 Olly Betts
 * Copyright 2010 Richard Boulton
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <config.h>

#include "api_replicate.h"

#include <xapian.h>

#include "apitest.h"
#include "dbcheck.h"
#include "safeerrno.h"
#include "safefcntl.h"
#include "safesysstat.h"
#include "safeunistd.h"
#include "str.h"
#include "testsuite.h"
#include "testutils.h"
#include "utils.h"
#include "unixcmds.h"

#include <sys/types.h>

#include <cstdlib>
#include <string>

using namespace std;

static void rmtmpdir(const string & path) {
    rm_rf(path);
}

static void mktmpdir(const string & path) {
    rmtmpdir(path);
    if (mkdir(path, 0700) == -1 && errno != EEXIST) {
	FAIL_TEST("Can't make temporary directory");
    }
}

static off_t file_size(const string & path) {
    struct stat sb;
    if (stat(path.c_str(), &sb)) {
	FAIL_TEST("Can't stat '" + path + "'");
    }
    return sb.st_size;
}

static size_t do_read(int fd, char * p, size_t desired)
{
    size_t total = 0;
    while (desired) {
	ssize_t c = read(fd, p, desired);
	if (c == 0) return total;
	if (c < 0) {
	    if (errno == EINTR) continue;
	    FAIL_TEST("Error reading from file");
	}
	p += c;
	total += c;
	desired -= c;
    }
    return total;
}

static void do_write(int fd, const char * p, size_t n)
{
    while (n) {
	ssize_t c = write(fd, p, n);
	if (c < 0) {
	    if (errno == EINTR) continue;
	    FAIL_TEST("Error writing to file");
	}
	p += c;
	n -= c;
    }
}

// Make a truncated copy of a file.
static off_t
truncated_copy(const string & srcpath, const string & destpath, off_t tocopy)
{
    int fdin = open(srcpath.c_str(), O_RDONLY);
    if (fdin == -1) {
	FAIL_TEST("Open failed (when opening '" + srcpath + "')");
    }

    int fdout = open(destpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fdout == -1) {
	FAIL_TEST("Open failed (when creating '" + destpath + "')");
    }

#define BUFSIZE 1024
    char buf[BUFSIZE];
    size_t total_bytes = 0;
    while (tocopy > 0) {
	size_t thiscopy = tocopy > BUFSIZE ? BUFSIZE : tocopy;
	size_t bytes = do_read(fdin, buf, thiscopy);
	if (thiscopy != bytes) {
	    FAIL_TEST("Couldn't read desired number of bytes from changeset");
	}
	tocopy -= bytes;
	total_bytes += bytes;
	do_write(fdout, buf, bytes);
    }
#undef BUFSIZE

    close(fdin);
    close(fdout);

    return total_bytes;
}

// Replicate from the master to the replica.
// Returns the number of changsets which were applied.
static void
get_changeset(const string & changesetpath,
	      Xapian::DatabaseMaster & master,
	      Xapian::DatabaseReplica & replica,
	      int expected_changesets,
	      int expected_fullcopies,
	      bool expected_changed)
{
    int fd = open(changesetpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd == -1) {
	FAIL_TEST("Open failed (when creating a new changeset file at '"
		  + changesetpath + "')");
    }
    fdcloser fdc(fd);
    Xapian::ReplicationInfo info1;
    master.write_changesets_to_fd(fd, replica.get_revision_info(), &info1);

    TEST_EQUAL(info1.changeset_count, expected_changesets);
    TEST_EQUAL(info1.fullcopy_count, expected_fullcopies);
    TEST_EQUAL(info1.changed, expected_changed);
}

static int
apply_changeset(const string & changesetpath,
		Xapian::DatabaseReplica & replica,
		int expected_changesets,
		int expected_fullcopies,
		bool expected_changed)
{
    int fd = open(changesetpath.c_str(), O_RDONLY);
    if (fd == -1) {
	FAIL_TEST("Open failed (when reading changeset file at '"
		  + changesetpath + "')");
    }
    fdcloser fdc(fd);

    int count = 1;
    replica.set_read_fd(fd);
    Xapian::ReplicationInfo info1;
    Xapian::ReplicationInfo info2;
    bool client_changed = false;
    while (replica.apply_next_changeset(&info2, 0)) {
	++count;
	info1.changeset_count += info2.changeset_count;
	info1.fullcopy_count += info2.fullcopy_count;
	if (info2.changed)
	    client_changed = true;
    }
    info1.changeset_count += info2.changeset_count;
    info1.fullcopy_count += info2.fullcopy_count;
    if (info2.changed)
	client_changed = true;

    TEST_EQUAL(info1.changeset_count, expected_changesets);
    TEST_EQUAL(info1.fullcopy_count, expected_fullcopies);
    TEST_EQUAL(client_changed, expected_changed);
    return count;
}

static int
replicate(Xapian::DatabaseMaster & master,
	  Xapian::DatabaseReplica & replica,
	  const string & tempdir,
	  int expected_changesets,
	  int expected_fullcopies,
	  bool expected_changed)
{
    string changesetpath = tempdir + "/changeset";
    get_changeset(changesetpath, master, replica,
		  expected_changesets,
		  expected_fullcopies,
		  expected_changed);
    return apply_changeset(changesetpath, replica,
			   expected_changesets,
			   expected_fullcopies,
			   expected_changed);
}

// Check that the databases held at the given path are identical.
static void
check_equal_dbs(const string & masterpath, const string & replicapath)
{
    Xapian::Database master(masterpath);
    Xapian::Database replica(replicapath);

    TEST_EQUAL(master.get_uuid(), master.get_uuid());
    dbcheck(replica, master.get_doccount(), master.get_lastdocid());

    for (Xapian::TermIterator t = master.allterms_begin();
	 t != master.allterms_end(); ++t) {
	TEST_EQUAL(postlist_to_string(master, *t),
		   postlist_to_string(replica, *t));
    }
}

static void
set_max_changesets(int count) {
#ifdef __WIN32__
    _putenv(("XAPIAN_MAX_CHANGESETS=" + str(count)).c_str());
#else
    setenv("XAPIAN_MAX_CHANGESETS", str(count).c_str(), 1);
#endif
}

// #######################################################################
// # Tests start here

// Basic test of replication functionality.
DEFINE_TESTCASE(replicate1, replicas) {
    string tempdir = ".replicatmp";
    mktmpdir(tempdir);
    string masterpath = get_named_writable_database_path("master");

    set_max_changesets(10);

    Xapian::WritableDatabase orig(get_named_writable_database("master"));
    Xapian::DatabaseMaster master(masterpath);
    string replicapath = tempdir + "/replica";
    Xapian::DatabaseReplica replica(replicapath);

    // Add a document to the original database.
    Xapian::Document doc1;
    doc1.set_data(string("doc1"));
    doc1.add_posting("doc", 1);
    doc1.add_posting("one", 1);
    orig.add_document(doc1);
    orig.commit();

    // Apply the replication - we don't have changesets stored, so this should
    // just do a database copy, and return a count of 1.
    int count = replicate(master, replica, tempdir, 0, 1, 1);
    TEST_EQUAL(count, 1);
    {
	Xapian::Database dbcopy(replicapath);
	TEST_EQUAL(orig.get_uuid(), dbcopy.get_uuid());
    }

    // Repeating the replication should return a count of 1, since no further
    // changes should need to be applied.
    count = replicate(master, replica, tempdir, 0, 0, 0);
    TEST_EQUAL(count, 1);
    {
	Xapian::Database dbcopy(replicapath);
	TEST_EQUAL(orig.get_uuid(), dbcopy.get_uuid());
    }

    // Regression test - if the replica was reopened, a full copy always used
    // to occur, whether it was needed or not.  Fixed in revision #10117.
    replica.close();
    replica = Xapian::DatabaseReplica(replicapath);
    count = replicate(master, replica, tempdir, 0, 0, 0);
    TEST_EQUAL(count, 1);
    {
	Xapian::Database dbcopy(replicapath);
	TEST_EQUAL(orig.get_uuid(), dbcopy.get_uuid());
    }

    orig.add_document(doc1);
    orig.commit();
    orig.add_document(doc1);
    orig.commit();

    count = replicate(master, replica, tempdir, 2, 0, 1);
    TEST_EQUAL(count, 3);
    {
	Xapian::Database dbcopy(replicapath);
	TEST_EQUAL(orig.get_uuid(), dbcopy.get_uuid());
    }

    check_equal_dbs(masterpath, replicapath);

    // Need to close the replica before we remove the temporary directory on
    // Windows.
    replica.close();
    rmtmpdir(tempdir);
    return true;
}

// Test replication from a replicated copy.
DEFINE_TESTCASE(replicate2, replicas) {
    SKIP_TEST_FOR_BACKEND("brass"); // Brass doesn't currently support this.

    string tempdir = ".replicatmp";
    mktmpdir(tempdir);
    string masterpath = get_named_writable_database_path("master");

    set_max_changesets(10);

    Xapian::WritableDatabase orig(get_named_writable_database("master"));
    Xapian::DatabaseMaster master(masterpath);
    string replicapath = tempdir + "/replica";
    Xapian::DatabaseReplica replica(replicapath);

    Xapian::DatabaseMaster master2(replicapath);
    string replica2path = tempdir + "/replica2";
    Xapian::DatabaseReplica replica2(replica2path);

    // Add a document to the original database.
    Xapian::Document doc1;
    doc1.set_data(string("doc1"));
    doc1.add_posting("doc", 1);
    doc1.add_posting("one", 1);
    orig.add_document(doc1);
    orig.commit();

    // Apply the replication - we don't have changesets stored, so this should
    // just do a database copy, and return a count of 1.
    TEST_EQUAL(replicate(master, replica, tempdir, 0, 1, 1), 1);
    check_equal_dbs(masterpath, replicapath);

    // Replicate from the replica.
    TEST_EQUAL(replicate(master2, replica2, tempdir, 0, 1, 1), 1);
    check_equal_dbs(masterpath, replica2path);

    orig.add_document(doc1);
    orig.commit();
    orig.add_document(doc1);
    orig.commit();

    // Replicate from the replica - should have no changes.
    TEST_EQUAL(replicate(master2, replica2, tempdir, 0, 0, 0), 1);
    check_equal_dbs(replicapath, replica2path);

    // Replicate, and replicate from the replica - should have 2 changes.
    TEST_EQUAL(replicate(master, replica, tempdir, 2, 0, 1), 3);
    check_equal_dbs(masterpath, replicapath);
    TEST_EQUAL(replicate(master2, replica2, tempdir, 2, 0, 1), 3);
    check_equal_dbs(masterpath, replica2path);

    // Stop writing changesets, and make a modification
    set_max_changesets(0);
    orig.close();
    orig = get_writable_database_again();
    orig.add_document(doc1);
    orig.commit();

    // Replication should do a full copy.
    TEST_EQUAL(replicate(master, replica, tempdir, 0, 1, 1), 1);
    check_equal_dbs(masterpath, replicapath);
    TEST_EQUAL(replicate(master2, replica2, tempdir, 0, 1, 1), 1);
    check_equal_dbs(masterpath, replica2path);

    // Start writing changesets, but only keep 1 in history, and make a
    // modification.
    set_max_changesets(1);
    orig.close();
    orig = get_writable_database_again();
    orig.add_document(doc1);
    orig.commit();

    // Replicate, and replicate from the replica - should have 1 changes.
    TEST_EQUAL(replicate(master, replica, tempdir, 1, 0, 1), 2);
    check_equal_dbs(masterpath, replicapath);
    TEST_EQUAL(replicate(master2, replica2, tempdir, 1, 0, 1), 2);
    check_equal_dbs(masterpath, replica2path);

    // Make two changes - only one changeset should be preserved.
    orig.add_document(doc1);
    orig.commit();

    // Replication should do a full copy, since one of the needed changesets
    // is missing.

    //FIXME - the following tests are commented out because the backends don't currently tidy up old changesets correctly.
    //TEST_EQUAL(replicate(master, replica, tempdir, 0, 1, 1), 1);
    //check_equal_dbs(masterpath, replicapath);
    //TEST_EQUAL(replicate(master2, replica2, tempdir, 0, 1, 1), 1);
    //check_equal_dbs(masterpath, replica2path);

    // Need to close the replicas before we remove the temporary directory on
    // Windows.
    replica.close();
    replica2.close();
    rmtmpdir(tempdir);
    return true;
}

static void
replicate_with_brokenness(Xapian::DatabaseMaster & master,
			  Xapian::DatabaseReplica & replica,
			  const string & tempdir,
			  int expected_changesets,
			  int expected_fullcopies,
			  bool expected_changed)
{
    string changesetpath = tempdir + "/changeset";
    get_changeset(changesetpath, master, replica,
		  1, 0, 1);

    // Try applying truncated changesets of various different lengths.
    string brokenchangesetpath = tempdir + "/changeset_broken";
    off_t filesize = file_size(changesetpath);
    off_t len = 10;
    off_t copylen;
    while (len < filesize) {
	copylen = truncated_copy(changesetpath, brokenchangesetpath, len);
	TEST_EQUAL(copylen, len);
	tout << "Trying replication with a changeset truncated to " << len <<
		" bytes, from " << filesize << " bytes\n";
	TEST_EXCEPTION(Xapian::NetworkError,
		       apply_changeset(brokenchangesetpath, replica,
				       expected_changesets, expected_fullcopies,
				       expected_changed));
	if (len < 30 || len >= filesize - 10) {
	    // For lengths near the beginning and end, increment size by 1
	    len += 1;
	} else {
	    // Don't bother incrementing by small amounts in the middle of
	    // the changeset.
	    len += 1000;
	    if (len >= filesize - 10) {
		len = filesize - 10;
	    }
	}
    }
    return;
}

// Test changesets which are truncated (and therefore invalid).
DEFINE_TESTCASE(replicate3, replicas) {
    string tempdir = ".replicatmp";
    mktmpdir(tempdir);
    string masterpath = get_named_writable_database_path("master");

    set_max_changesets(10);

    Xapian::WritableDatabase orig(get_named_writable_database("master"));
    Xapian::DatabaseMaster master(masterpath);
    string replicapath = tempdir + "/replica";
    Xapian::DatabaseReplica replica(replicapath);

    // Add a document to the original database.
    Xapian::Document doc1;
    doc1.set_data(string("doc1"));
    doc1.add_posting("doc", 1);
    doc1.add_posting("one", 1);
    orig.add_document(doc1);
    orig.commit();

    TEST_EQUAL(replicate(master, replica, tempdir, 0, 1, 1), 1);
    check_equal_dbs(masterpath, replicapath);

    // Make a changeset.
    orig.add_document(doc1);
    orig.commit();

    replicate_with_brokenness(master, replica, tempdir, 1, 0, 1);
    // Although it throws an error, the final replication in
    // replicate_with_brokenness() updates the database, since it's just the
    // end-of-replication message which is missing its body.
    check_equal_dbs(masterpath, replicapath);

    // Check that the earlier broken replications didn't cause any problems for the
    // next replication.
    orig.add_document(doc1);
    orig.commit();
    TEST_EQUAL(replicate(master, replica, tempdir, 1, 0, 1), 2);

    // Need to close the replicas before we remove the temporary directory on
    // Windows.
    replica.close();
    rmtmpdir(tempdir);
    return true;
}

// Basic test of replication functionality.
DEFINE_TESTCASE(replicate4, replicas) {
    string tempdir = ".replicatmp";
    mktmpdir(tempdir);
    string masterpath = get_named_writable_database_path("master");

    set_max_changesets(10);

    Xapian::WritableDatabase orig(get_named_writable_database("master"));
    Xapian::DatabaseMaster master(masterpath);
    string replicapath = tempdir + "/replica";
    Xapian::DatabaseReplica replica(replicapath);

    // Add a document with no positions to the original database.
    Xapian::Document doc1;
    doc1.set_data(string("doc1"));
    doc1.add_term("nopos");
    orig.add_document(doc1);
    orig.commit();

    // Apply the replication - we don't have changesets stored, so this should
    // just do a database copy, and return a count of 1.
    int count = replicate(master, replica, tempdir, 0, 1, 1);
    TEST_EQUAL(count, 1);
    {
	Xapian::Database dbcopy(replicapath);
	TEST_EQUAL(orig.get_uuid(), dbcopy.get_uuid());
    }

    // Add a document with positional information to the original database.
    doc1.add_posting("pos", 1);
    orig.add_document(doc1);
    orig.commit();

    // Replicate, and check that we have the positional information.
    count = replicate(master, replica, tempdir, 1, 0, 1);
    TEST_EQUAL(count, 2);
    {
	Xapian::Database dbcopy(replicapath);
	TEST_EQUAL(orig.get_uuid(), dbcopy.get_uuid());
    }
    check_equal_dbs(masterpath, replicapath);

    // Need to close the replica before we remove the temporary directory on
    // Windows.
    replica.close();
    rmtmpdir(tempdir);
    return true;
}
