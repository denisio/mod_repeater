#!/usr/bin/perl -w

use IO::Socket;
use IO::Select;
use strict;

my $port = 10000;

unless (defined $ARGV[0] && defined $ARGV[1] && defined $ARGV[2] && defined $ARGV[3]) {
   print "The UDP to TCP converter is Copyright (C) 2010 Denis Erygin\n";
   print "and licensed under the GNU General Public License, version 2\n";
   print "Bug reports, feedback, admiration, abuse, etc, to: denis.erygin\@gmail.com\n\n";
   print "Usage: ./udp.pl bind_ip bind_port to_tcp_ip to_tcp_port\n";
   exit;
}

my $bind_ip     = $ARGV[0];
my $bind_port   = $ARGV[1]; 
my $to_tcp_ip   = $ARGV[2];
my $to_tcp_port = $ARGV[3];

my $srv = IO::Socket::INET->new( LocalHost => $bind_ip,
                                 LocalPort => $bind_port,
                                 Proto     => 'udp',
                                 Type      => SOCK_DGRAM,
                                 Reuse     => 1,
                                 Broadcast => 0)
            or die "Couldn't be a udp server on port $port: $!\n";
          

my $select = IO::Select->new($srv);

my $count = 0;
while ($srv)
{
   for my $client ($select->can_read())
   {
      if ($client == $srv)
      {
         my $data = '';
         my $rv   = $client->recv($data, 4096, 0);
         unless ( defined $rv && length($data) ) {
            $select->remove($client);
            close($srv);
            $srv = undef;
            last;
         }
         
         $count++;
         print "Send requests:\t\b".$count,"\n" if ($count%100 == 0);
         
         send_request($to_tcp_ip, $to_tcp_port, $data);
      }
   }   
}


sub send_request 
{
  my ($host, $port, $data) = @_;
  return undef unless($host && $port && $data);
    
  my $sock = IO::Socket::INET->new( PeerHost => $host,
                                    PeerPort => $port,
                                    Proto    => 'tcp',
                                    Timeout  => 30 )
             or die "Couldn't connect: $@";

  $data =~ s/\r\n$/Host: $host:$port\r\n\r\n/;
  
  eval {
     $sock->send($data);
  };
  if($@) {
     print $@,"\n";
  }
  
  close($sock);
}

