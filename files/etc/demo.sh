# TinyOS shell script demo + regression fixture.
# Run it with:  source etc/demo.sh
# Lines starting with '#' and blank lines are skipped.  The same parser
# serves the console: every command below can also be typed at the prompt,
# except the block statements, which need a file to jump back in.

name=$(echo tiny)
echo [script] name=$name
if $name == tiny
    echo [script] if-ok
else
    echo [script] if-BAD
fi

# $(...) points fd 1 at a pipe, spawns the program and reads it to EOF.
t=$(cat /proc/uptime)
echo [script] uptime=$t
if $t != 0
    echo [script] capture-ok
fi

# A program call and an ordinary two-program pipeline, for comparison.
hello
echo abc def | wc

# sleep is a builtin here: three iterations, three exec's avoided.
i=0
while $i -lt 3
    echo [script] loop $i
    incr i
    sleep 10
done
echo [script] after-loop i=$i
if $i -ne 3
    echo [script] loop-BAD
fi

# break leaves the innermost loop from inside an if.
j=0
while $j -lt 10
    incr j
    if $j -eq 2
        echo [script] break-at-$j
        break
    fi
done
echo [script] after-break j=$j

false
echo [script] false-status=$?
true
echo [script] done
