#!/usr/bin/env perl
# A stand-in for AeroSpace's server socket, for tests/test_aerospace_launcher.sh.
#
# Speaks the real protocol (Sources/Common/util/NWConnectionEx.swift,
# Sources/AppBundle/server.swift in AeroSpace): each side sends its UInt32
# protocol version, then every message is a native-endian UInt32 length and
# JSON. "subscribe" turns a connection into an event stream whose first
# event is focus-changed; the commands st-aerospace-launch uses answer with
# {"exitCode","stdout","stderr","serverVersionAndHash"}.
#
# Usage: fake_aerospace_server.pl SOCKET. Everything else is in files named
# by the environment, so a test can change it while the server runs:
#   FAKE_AEROSPACE_LOG     requests worth asserting on are logged here
#   FAKE_EVENTS            "detected PID" lines appended by fake spares; each
#                          becomes a window-detected event for window PID
#   FAKE_DETECT_WORKSPACE  where new windows are placed
#   FAKE_FOCUS_WORKSPACE   the focused workspace
#   FAKE_FOCUSED           `list-windows --focused` output
#   FAKE_ST_READY_DIR      PID files of spares that received their request
#   FAKE_NO_DETECT         if this file exists, windows are never detected
#   FAKE_FAIL_FOCUS        if this file exists, focus and move fail
#   FAKE_TEST_PID          the server exits once this process is gone

use strict;
use warnings;
use Encode qw(decode encode);
use IO::Select;
use IO::Socket::UNIX;
use JSON::PP;
use Socket qw(SOCK_STREAM);

my $path = shift or die "usage: $0 SOCKET\n";
$SIG{PIPE} = 'IGNORE'; # a launcher may go away mid-event
my $json = JSON::PP->new->utf8->canonical;
unlink $path;
my $server = IO::Socket::UNIX->new(Type => SOCK_STREAM, Local => $path,
    Listen => 128) or die "listen $path: $!";
my $select = IO::Select->new($server);
my %clients;

sub text
{
	my ($file) = @_;
	open(my $in, '<', $file) or return '';
	my $content = do { local $/; <$in> } // '';
	chomp $content;
	return decode('UTF-8', $content);
}

sub logline
{
	open(my $out, '>>', $ENV{FAKE_AEROSPACE_LOG}) or die "log: $!";
	print $out encode('UTF-8', "$_[0]\n");
}

sub send_frame
{
	my ($client, $message) = @_;
	my $payload = $json->encode($message);
	my $data = pack('L', length $payload) . $payload;
	while (length $data) {
		my $written = syswrite($client->{socket}, $data);
		return unless defined $written;
		substr($data, 0, $written) = '';
	}
}

sub answer
{
	my ($client, $code, $stdout) = @_;
	send_frame($client, {exitCode => $code, stdout => $stdout // '',
	    stderr => $client->{warning} // '',
	    serverVersionAndHash => 'fake'});
}

sub handle
{
	my ($client, $request) = @_;
	my @args = @{$request->{args}};
	my $command = shift @args // '';
	$client->{warning} = undef;
	if (!exists $request->{windowId} || !exists $request->{workspace}) {
		logline('incomplete-request');
		$client->{warning} = 'incomplete JSON request';
	}
	if ($command eq 'subscribe') {
		my %events = map { $_ => 1 } @args;
		die "unexpected subscription @args"
		    unless $events{'focus-changed'} && $events{'window-detected'};
		$client->{subscribed} = 1;
		send_frame($client, {_event => 'focus-changed', windowId => 1,
		    workspace => text($ENV{FAKE_FOCUS_WORKSPACE})});
	} elsif ($command eq 'list-workspaces') {
		answer($client, 0, text($ENV{FAKE_FOCUS_WORKSPACE}));
	} elsif ($command eq 'list-apps') {
		logline('list-apps');
		answer($client, 0, '');
	} elsif ($command eq 'list-windows' && grep { $_ eq '--focused' } @args) {
		logline(sprintf('focused-context %s %s',
		    $request->{windowId} // 'null', $request->{workspace} // 'null'));
		answer($client, 0, text($ENV{FAKE_FOCUSED}));
	} elsif ($command eq 'list-windows') {
		my ($pid) = map { $args[$_ + 1] } grep { $args[$_] eq '--pid' } 0 .. $#args;
		return answer($client, 2, '') unless defined $pid;
		logline("query $pid");
		my $found = -f "$ENV{FAKE_ST_READY_DIR}/$pid" && kill(0, $pid) &&
		    !-e $ENV{FAKE_NO_DETECT};
		answer($client, 0, $found ?
		    "$pid " . text($ENV{FAKE_DETECT_WORKSPACE}) : '');
	} elsif ($command eq 'move-node-to-workspace' &&
	    "@args[0, 1]" eq '--focus-follows-window --window-id' && @args == 4) {
		logline("move $args[2] $args[3]");
		answer($client, -e $ENV{FAKE_FAIL_FOCUS} ? 2 : 0, '');
	} elsif ($command eq 'focus' && @args == 2 && $args[0] eq '--window-id') {
		logline("focus $args[1]");
		answer($client, -e $ENV{FAKE_FAIL_FOCUS} ? 2 : 0, '');
	} else {
		logline("unexpected @{$request->{args}}");
		answer($client, 2, '');
	}
}

sub receive
{
	my ($client) = @_;
	if (!$client->{greeted}) {
		return if length $client->{buffer} < 4;
		my $version = unpack('L', substr($client->{buffer}, 0, 4, ''));
		syswrite($client->{socket}, pack('L', 1));
		return drop($client) if $version != 1;
		$client->{greeted} = 1;
	}
	while (!$client->{subscribed} && length $client->{buffer} >= 4) {
		my $size = unpack('L', $client->{buffer});
		last if length $client->{buffer} < 4 + $size;
		my $payload = substr($client->{buffer}, 0, 4 + $size, '');
		handle($client, $json->decode(substr($payload, 4)));
	}
	# Like the real server, a subscribed connection only waits for EOF.
	$client->{buffer} = '' if $client->{subscribed};
}

sub drop
{
	my ($client) = @_;
	$select->remove($client->{socket});
	delete $clients{fileno $client->{socket}};
	close $client->{socket};
}

open(my $events, '<', $ENV{FAKE_EVENTS}) or die "events: $!";
my $pending = '';
# Never outlive the test: stop when its shell is gone, and after a while.
my $test = $ENV{FAKE_TEST_PID};
my $deadline = time + 600;
for (;;) {
	if (time > $deadline || ($test && !kill(0, $test))) {
		unlink $path;
		exit 0;
	}
	for my $socket ($select->can_read(0.01)) {
		if ($socket == $server) {
			my $accepted = $server->accept or next;
			$select->add($accepted);
			$clients{fileno $accepted} = {socket => $accepted,
			    buffer => ''};
			next;
		}
		my $client = $clients{fileno $socket};
		my $got = sysread($socket, my $chunk, 65536);
		if (!$got) {
			drop($client);
			next;
		}
		$client->{buffer} .= $chunk;
		receive($client);
	}
	# Broadcast windows that appeared since the last pass.
	while (defined(my $line = <$events>)) {
		$pending .= $line;
		next unless $pending =~ s/\A(detected (\d+))\n//;
		next if -e $ENV{FAKE_NO_DETECT};
		my $event = {_event => 'window-detected', appBundleId => 'io.yeyito.st',
		    appName => 'st', windowId => $2 + 0,
		    workspace => text($ENV{FAKE_DETECT_WORKSPACE})};
		send_frame($_, $event) for grep { $_->{subscribed} } values %clients;
	}
	seek($events, 0, 1); # clear EOF so later appends are read
}
