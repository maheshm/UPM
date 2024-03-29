cd /home/atwork/project_UPM/final_code/UPM_root/usr/local/pkg/
targetdir=$"/home/atwork/project_UPM/final_code/actual_root/"
root=${1%/}
test -d $root || { echo "lt error: $root not a directory. abort." 1>&2; exit 1; }
root=${root##*/}
actual_root=$(pwd)/$root
cd $1/..
bdir=${PWD##*/}
target=$(cd $targetdir; pwd)
echo $target
echo $root
echo $bdir
dirs=$(find $root -type d)
for dir in $dirs
do
	$(echo $dir | sed "s:$root:mkdir -p $target:")
done

files=$(find $root ! -type d)

for file in $files
do
	file_dir=$(dirname $file)
	dst_dir=$(echo $file_dir|sed "s:$root::")
	$(echo $file | sed "s:$root:ln -s  -t $target$dst_dir ${actual_root}:")
done



