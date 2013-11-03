imagefile-partition
===================


Sometimes it is useful to create an image file with partitions inside it and it always required me to work as root which I would prefer to avoid. imagefile-partition is an attempt to make things work without being root.

imagefile-partition is an LD_PRELOAD hack that takes environment variables to define the image file and the partition and redirects the seeks and thus the read and write locations inside the area for the partition thus making it possible to create the filesystem only in the partition region.

### State

It shows the beginning of being able to work on a 64-bit Linux machine but still not perfectly so.
