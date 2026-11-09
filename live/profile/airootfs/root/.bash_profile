# tty1 shows the status screen until a key is pressed, then a shell; an ssh
# login prints it once. (Commands run over ssh, like the harness's, don't read
# this file.)
if [[ $(tty) == /dev/tty1 ]]; then
    ms-status --watch
elif [[ -n $SSH_TTY ]]; then
    ms-status
fi
