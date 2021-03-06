#! /usr/bin/python2.4
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#

#
# Copyright 2009 Cyril Plisko.  All rights reserved.
# Use is subject to license terms.
#
# Copyright 2010 Joyent, Inc.  All rights reserved.
# Use is subject to license terms.
#

'''
Create a wx-style active list on stdout based on a Git
workspace in support of webrev's Git support.
'''

import os
import sys
import optparse
import subprocess

def execCmd(cmd):
    '''Executes external command'''

    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE)
    (out, err) = p.communicate()

    if out == None:
        out = ""

    return (p.returncode, out.splitlines(), err.splitlines())

def main(argv):

    usage = "usage: %prog [-p parent] -w workspace"
    parser = optparse.OptionParser(version="%prog 1.0", usage=usage)
    parser.add_option("-p", dest="parentpath", default="", help="parent repo")
    parser.add_option("-w", dest="wspath", default="", help="workspace")
    parser.add_option("-o", dest="outputfile", help="output file")
    parser.disable_interspersed_args()

    (options, args) = parser.parse_args()

    if not options.wspath:
        parser.print_help()
        sys.exit(2)

    fh = None
    if options.outputfile:
        try:
            fh = open(options.outputfile, 'w')
        except EnvironmentError, e:
            sys.stderr.write("could not open output file: %s\n" % e)
            sys.exit(1)
    else:
        fh = sys.stdout
 
    branch = "master"
    cmd = ["git", "branch"]
    (rc, out, err) = execCmd(cmd)
    for i in out:
        if i.startswith("*"):
            branch = i.split()[1]

    cmd = ["git", "push", "--dry-run", "origin", "%s" % branch]
    (rc, out, err) = execCmd(cmd)
    for i in err:
        if i.startswith("To "):
            continue
        rev = i.split()[0]

    if rev == "":
        sys.exit(0)
 
    cmd = ["git", "--git-dir=%s" % options.wspath, "log", "--name-only",
         "--parents", "--reverse", "--pretty=short", "%s" % rev]
    (rc, out, err) = execCmd(cmd)

    if out == []:
        sys.exit(0)
 
    if "commit" in out[0]:
        parent = out[0].split()[2]

    files = {}
    comment = None
    for i in out:
        if "commit" in i:
            comment = None
            continue
        if i == "" or i.startswith("Author"):
            continue
        if i.startswith("    "):
            comment = i.strip()
            continue
        if comment:
            if i not in files:
                files[i] = []
            files[i].append(comment)
 
    fh.write("GIT_PARENT=%s\n" % parent)
 
    for file in sorted(files.iterkeys()):
        #if entry.is_renamed():
        #    fh.write("%s %s\n" % (entry.name, entry.parentname))
        #else:
        #    fh.write("%s\n" % entry.name)
        fh.write("%s\n\n" % file)
        fh.write("%s\n\n" % '\n'.join(files[file]))
 
if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except KeyboardInterrupt:
        sys.exit(1)
