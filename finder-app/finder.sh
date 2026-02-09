#!/bin/sh

filesdir=$1
searchstr=$2

if [ -z $filesdir -o -z $searchstr ]; then
	echo 'Must specify directory and searchstring!'
	exit 1
elif [ ! -d $filesdir ]; then
	echo "$filesdir is not a directory!"
	exit 1
fi

# only file types
num_files=$(find $filesdir/ -type f | wc -l)
# skip subdirectories
num_lines=$(grep -r $searchstr $filesdir/* | wc -l)

echo "The number of files are $num_files and the number of matching lines are $num_lines"
