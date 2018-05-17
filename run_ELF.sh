#!/bin/bash
source /home/lucas_jd_baker/.bashrc
cd ~/ELF 
source scripts/devmode_set_pythonpath.sh
cd scripts/elfgames/go
export PATH='/home/lucas_jd_baker/bin:/home/lucas_jd_baker/.local/bin:/home/lucas_jd_baker/anaconda3/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games:/usr/local/games:/snap/bin'
export PYTHONPATH='/home/lucas_jd_baker/ELF/src_py/:/home/lucas_jd_baker/ELF/build/elf/:/home/lucas_jd_baker/ELF/build/elfgames/go/:/home/lucas_jd_baker/ELF/src_py/:/home/lucas_jd_baker/ELF/build/elf/:/home/lucas_jd_baker/ELF/build/elfgames/go/'
export XDG_DATA_DIRS='/usr/local/share:/usr/share:/var/lib/snapd/desktop'
./gtp.sh /home/lucas_jd_baker/ELF/pretrained-go-19x19-v0.bin --verbose --gpu 1 --num_block 20 --dim 224 --mcts_puct 1.50 --batchsize 16 --mcts_rollout_per_batch 16 --mcts_threads 2 --mcts_rollout_per_thread 1000 --resign_thres 0.05 --mcts_virtual_loss 1
