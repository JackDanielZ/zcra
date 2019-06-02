# ZCRA

Tool to automate programs executions remotely via predefined scripts


### Installing

make

## Checking it works

build/bin/zcra

## Scripts

Scripts should be located in the .config/zcra/scripts/ directory and protected from others access (chmod 600)

The syntax is as follows:
- TYPE str: sends the string str to the program as if the user were typing it (stdin)
- WAIT str: waits for the string str to be displayed by the program. Useful to wait for a command to finish, a password to be requested...
- PASSWORD alias: sends the password whose alias is given to the program. The password should be stored in the .config/zcra/passwords directory (chmod 700) into the file 'alias' (chmod 600)

Applying a script is done remotely via UDP. The program should be executed with the option --id *nb* in order to get the UDP binding made on port 40000 + *nb*.

Example:
```
zcra --id 1 -- program [args]
echo "script name" > /dev/udp/127.0.0.1/40001
```
