namespace eval expectnmcu::net {
}

package require expectnmcu::core

# Wait for `wifi.sta.getip()` to return something that looks like an address,
# indicating that the device under test is probably online.
proc ::expectnmcu::net::waitwifista { dev {timeout 10} } {
  for {set i 0} {${i} < ${timeout}} {incr i} {
    send -i ${dev} "=wifi.sta.getip()\n"
    expect {
      -i ${dev} -re "\n(\[^\n\t\]+)\t\[^\t\]+\t\[^\t\]+\[\r\n\]+> " {
        # That looks like an address report to me
        return ${expect_out(1,string)}
      }
      -i ${dev} -ex "nil\[\r\n\]+> " {
        # must not be connected yet
        sleep 1
      }
      -i ${dev} -re ${::expectnmcu::core::panicre} { return -code error "Panic!" }
      timeout { }
    }
  }

  return -code error "WIFI STA: no IP address"
}

proc ::expectnmcu::net::guessmyip { victimip } {
  # Guess our IP address by using the victim's
  spawn "ip" "route" "get" ${victimip}
  expect {
    -re "src (\[0-9.\]+) " {
      close
      return ${expect_out(1,string)}
    }
  }
  close
  return -code error "Cannot find source IP"
}

package provide expectnmcu::net 1.0
