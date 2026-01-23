#!/bin/bash

writefile=$1
writestr=$2

if [ -z "$writefile" -o -z "$writestr" ]; then
	echo 'Must specify file-to-write and content to be written!'
	exit 1
fi

echo "$writestr" > "$writefile"

if [ $? -eq 1 ]; then
	echo "Error: Could not create file $writefile."
	exit 1
fi
