# Perl fork() emulation stress, modelled after how mysql-test-run.pl starts
# its workers: N forked pseudo-processes talking to the parent over sockets,
# parent selects/accepts and then waits for all of them.
#   perl forktest.pl N [spawn]
# "spawn": every child also starts a few real processes, like mtr workers do.
use strict; use warnings; use IO::Select; use IO::Socket::INET;
$| = 1;
my ($n, $spawn) = @ARGV;
my $server = IO::Socket::INET->new(Listen => $n, LocalAddr => '127.0.0.1',
                                   LocalPort => 0, Proto => 'tcp')
  or die "listen: $!";
$server->timeout(20);
my $port = $server->sockport;
my @pids;
for my $i (1 .. $n) {
  my $pid = fork();
  die "fork $i failed: $!\n" unless defined $pid;
  if ($pid == 0) {
    my $s = IO::Socket::INET->new(PeerAddr => '127.0.0.1', PeerPort => $port,
                                  Proto => 'tcp') or exit 100;
    if ($spawn) { system("cmd", "/c", "exit", "0") for 1 .. 3; }
    print $s "hello $i\n"; $s->flush; sleep 1; close $s; exit 0;
  }
  push @pids, $pid;
}
my $sel = IO::Select->new; my $got = 0;
while ($got < $n) {
  my $a = $server->accept or last;
  $sel->add($a); $got++;
}
my $readable = () = $sel->can_read(5);
my $bad = 0;
for my $pid (@pids) { waitpid($pid, 0); $bad++ if $? != 0 }
printf "N=%d spawn=%d forked=%d accepted=%d readable=%d badexit=%d %s\n",
  $n, $spawn ? 1 : 0, scalar(@pids), $got, $readable, $bad,
  ($got == $n && !$bad) ? "OK" : "PROBLEM";
exit(($got == $n && !$bad) ? 0 : 1);
