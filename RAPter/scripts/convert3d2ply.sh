#!/bin/bash

function print_usage() {
        echo "usage:\t convert3d2ply.sh output.ply"
        echo "i.e. /home/bontius/workspace/globOpt/globOpt/scripts/convert3d2ply.sh koblenz.ply"
}

if [[ -z "$1" ]]; then
	print_usage
	exit 1
else
	out=$1;
fi

rm $out

for f in `ls *.3d`; do
	awk '{print $1,$2,$3}' $f >> "_tmp.ply";
done

numv=`wc -l < _tmp.ply`
echo "numv: " $numv

# append ply header
echo "ply" >>$out
echo "format ascii 1.0"  >>$out
echo "comment Aron generated by convert3d2ply.sh"  >>$out
echo "element vertex $numv"  >>$out
echo "property float x"  >>$out
echo "property float y"  >>$out
echo "property float z" >>$out
#property float nx
#property float ny
#property float nz
#property float curvature
echo "end_header" >>$out
cat "_tmp.ply" >>$out

rm "_tmp.ply"

# Then run:
# /home/bontius/workspace/globOpt/globOpt/build/Release/bin/glob_opt --subsample --N 0 --scene-size 0 --cloud koblenz.ply --origin 0,0,0