# Unix SMB/CIFS implementation.
# Copyright (C) Sean Dague <sdague@linux.vnet.ibm.com> 2011
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

# This provides a wrapper around the cmd interface so that tests can
# easily be built on top of it and have minimal code to run basic tests
# of the commands. A list of the environmental variables can be found in
# ~/selftest/selftest.pl
#
# These can all be accessed via os.environ["VARIABLENAME"] when needed

import os
import random
import string
from samba.auth import system_session
from samba.samdb import SamDB
from io import StringIO
from samba.netcmd.main import cmd_sambatool
from optparse import OptionParser
import samba.getopt as options
import samba.tests


def truncate_string(s, cutoff=100):
    if len(s) < cutoff + 15:
        return s
    return s[:cutoff] + '[%d more characters]' % (len(s) - cutoff)


class SambaToolCmdTest(samba.tests.BlackboxTestCase):
    # Use a class level reference to StringIO, which subclasses can
    # override if they need to (to e.g. add a lying isatty() method).
    stringIO = StringIO

    @staticmethod
    def getSamDB(*argv):
        """a convenience function to get a samdb instance so that we can query it"""

        parser = OptionParser()
        sambaopts = options.SambaOptions(parser)
        credopts = options.CredentialsOptions(parser)
        parser.add_option("-H", "--URL",
                          help="LDB URL for database or target server",
                          type=str, metavar="URL", dest="H")
        opts, args = parser.parse_args(list(argv))

        lp = sambaopts.get_loadparm()
        creds = credopts.get_credentials(lp, fallback_machine=True)

        return SamDB(url=opts.H, session_info=system_session(),
                     credentials=creds, lp=lp)

    @classmethod
    def _run(cls, *argv):
        """run a samba-tool command"""
        cmd, args = cmd_sambatool()._resolve('samba-tool', *argv,
                                             outf=cls.stringIO(),
                                             errf=cls.stringIO())
        result = cmd._run(*args)
        return (result, cmd.outf.getvalue(), cmd.errf.getvalue())

    runcmd = _run
    runsubcmd = _run

    def runsublevelcmd(self, name, sublevels, *args):
        """run a command with any number of sub command levels"""
        # This is a weird and clunky interface for running a
        # subcommand. Use self.runcmd() instead.
        return self._run(name, *sublevels, *args)

    def assertCmdSuccess(self, exit, out, err, msg=""):
        # Make sure we allow '\n]\n' in stdout and stderr
        # without causing problems with the subunit protocol.
        # We just inject a space...
        msg = "exit[%s] stdout[%s] stderr[%s]: %s" % (exit, out, err, msg)
        self.assertIsNone(exit, msg=msg.replace("\n]\n", "\n] \n"))

    def assertCmdFail(self, val, msg=""):
        self.assertIsNotNone(val, msg)

    def assertMatch(self, base, string, msg=None):
        # Note: we should stop doing this and just use self.assertIn()
        if msg is None:
            msg = "%r is not in %r" % (truncate_string(string),
                                       truncate_string(base))
        self.assertIn(string, base, msg)

    def randomName(self, count=8):
        """Create a random name, cap letters and numbers, and always starting with a letter"""
        name = random.choice(string.ascii_uppercase)
        name += ''.join(random.choice(string.ascii_uppercase + string.ascii_lowercase + string.digits) for x in range(count - 1))
        return name

    def randomXid(self):
        # pick some unused, high UID/GID range to avoid interference
        # from the system the test runs on

        # initialize a list to store used IDs
        try:
            self.used_xids
        except AttributeError:
            self.used_xids = []

        # try to get an unused ID
        failed = 0
        while failed < 50:
            xid = random.randint(4711000, 4799000)
            if xid not in self.used_xids:
                self.used_xids += [xid]
                return xid
            failed += 1
        assert False, "No Xid are available"

    def assertWithin(self, val1, val2, delta, msg=""):
        """Assert that val1 is within delta of val2, useful for time computations"""
        self.assertTrue(((val1 + delta) > val2) and ((val1 - delta) < val2), msg)

    def cleanup_join(self, netbios_name):
        (result, out, err) \
            = self.runsubcmd("domain",
                             "demote",
                             ("--remove-other-dead-server=%s " % netbios_name),
                             ("-U%s%%%s" % (os.environ["USERNAME"], os.environ["PASSWORD"])),
                             ("--server=%s" % os.environ["SERVER"]))

        self.assertCmdSuccess(result, out, err)
