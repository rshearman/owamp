#!/usr/local/bin/perl -lw

# File: nodemaster.pl
# Author: Anatoly Karp, Internet2 2002
# $Id$

# This script manages uploading new data files from a given directory 
# (or current directory, if omitted) to the central host (also referred to
# as remote host) and checks their integrity using md5 hash.
# Read configuration section to fine-tune scripts behaviour.

# Usage: nodemaster.pl [dirname]

use strict;
use constant DEBUG => 1;

### Start of configuration section. Change these values as appropriate.

my $local_md5_path = '/sbin/md5';           # path to local md5 program
my $remote_md5_path = '/usr/bin/md5sum';    # same on the remote host
my $remote_user = 'karp\@sss.advanced.org'; # argument to scp
my $remote_dir = 'datadir';                 # remote dir to place the files
                                            # NOTE: make sure it exists!

my $data_glob = '*.owp';                    # shell glob to match data files

# due to variety of md5 programs, the fields in which the md5 is found
# may vary. For example', FreeBSD's output of md5 program may look so:
# bash-2.05a$ md5 file
# MD5 (file) = ff68e78adcc3c8026174d95f4f9d1478

# while Debian's md5sum program yields:
# bash$ md5sum file
# ff68e78adcc3c8026174d95f4f9d1478  file

# the next two values give the field number (starting from 0)
# in which the md5 value for the corresponding host to be found:
my $local_md5_field = 3;
my $remote_md5_field = 0;

### End of configuration section.

my $dirname = $ARGV[0] || '.';
chdir $dirname or die "could not chdir: $!";

while (1) {  # look for matching files and send them off - round-robin style
  my $files = qx/ls $data_glob/;

  unless ($files) {
    warn "no matching filenames found";
    die "DEBUG" if 1;               # XXX - comment out eventually
    sleep 60;
    next;
  }

  print $files if DEBUG;

  my @files = split /\s/, $files;
  foreach my $file (@files) {
    push_try($file);
  }
}

# this sub attempts to transfer the file to the remote host
# and deletes it on successf
sub push_try {
  my $filename = $_[0];             # datafile to be transferred to remote host
  my $md5_string = qx/$local_md5_path $filename/;
  my @res = split(' ', $md5_string);
  chomp $res[$local_md5_field];
  unlink $filename if push_ok($res[$local_md5_field], $remote_dir, $filename);
}

# try to scp a given file to the remote host and check its md5 value
# agains the given one (presumably the one computed on localhost)
sub push_ok {
  my ($md5_loc, $rem_dir, $file) = @_;

  my $cmd = join(' ', 'scp', $file, "$remote_user:$rem_dir", '>/dev/null');
  print "DEBUG: local md5 = $md5_loc" if DEBUG;
  print "DEBUG: cmd = ", $cmd if DEBUG;
  if (system($cmd) > 0) {
    warn "system() failed: $!";
    return undef;
  }
  
  my $relative_filename = $file;
  if ($rem_dir ne '') {
    $relative_filename = "$rem_dir/$file";
  }

  print "DEBUG: remote relative filename = ", $relative_filename if DEBUG;

  my $out = qx/ssh $remote_user $remote_md5_path $relative_filename/;
  return undef unless $out;

  my @res = split /\s/, $out;

  print "DEBUG: remote md5 = $res[$remote_md5_field]" if DEBUG;
  print "" if DEBUG;

  return ($md5_loc eq $res[$remote_md5_field])? 1 : undef;
}

