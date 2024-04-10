#!/bin/bash

basedir=$(cd `dirname $0`;pwd)
instdir=$basedir/..
ERRFILE=$instdir/err.log

. $basedir/utils.sh

service pccs start &>>$ERRFILE
if [ $? -ne 0 ]; then
    verbose ERROR "Failed to start service pccs. Check $ERRFILE for more details." t
    return 0
else
    verbose INFO "Service pccs started successfully." t
fi

# Start dcap service, must change the seed string for testing
/opt/crust/tools/bin/dcap-service -p 17777 -s 1111111111111111111111111111111111111111111111111111111111111111
