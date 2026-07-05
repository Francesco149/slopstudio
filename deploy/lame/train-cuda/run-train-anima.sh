#!/usr/bin/env bash
# Train a Gemma-san character LoRA on **Anima** (DiT / Rectified-Flow, 2B) — runs INSIDE the
# slop-train-cuda container on lame's RTX 3080 (CUDA bf16; ROCm NaN bug dodged). Anima has
# native kohya support (anima_train_network.py, networks.lora_anima).
#
#   docker exec -e MAX_STEPS=30 -i slop-train-cuda bash /app/run-train-anima.sh   # VALIDATE
#   docker exec -i slop-train-cuda bash /app/run-train-anima.sh                    # FULL
#
# Mounts (from the recreated container): /models (the whole comfy models tree, ro),
# /dataset (the shared Gemma chibi ref set + WD14 captions), /output (= ComfyUI models/loras).
#
# Captions: REUSE the existing pruned WD14 tags in /dataset/*.txt (same set that trained the
# verified Illustrious v2 LoRA) WITHOUT re-running WD14 (don't clobber the shared dataset). We
# only re-prune (safety) + swap the trigger: drop {gemma-san, chibi} + design tags, prepend
# the unique Anima trigger so it OWNS the character + flat-cel look. Built into _train_anima/
# so the shared /dataset/*.txt stay untouched.
#
# Config via env (defaults = the confirmed-fitting 3080 recipe):
#   LORA_NAME TRIGGER DIM ALPHA LR REPEATS EPOCHS SAVE_EVERY RES BLOCKS_SWAP MAX_STEPS(validate)
set -euo pipefail

DATA=/dataset
DIT="${DIT:-/models/diffusion_models/anima-base-v1.0.safetensors}"
QWEN3="${QWEN3:-/models/text_encoders/qwen_3_06b_base.safetensors}"
VAE="${VAE:-/models/vae/qwen_image_vae.safetensors}"
OUT=/output
NAME="${LORA_NAME:-gemma-san-anima}"
TRIGGER="${TRIGGER:-gemma-san-anima}"
REPEATS="${REPEATS:-4}"
EPOCHS="${EPOCHS:-10}"
SAVE_EVERY="${SAVE_EVERY:-2}"
DIM="${DIM:-32}"; ALPHA="${ALPHA:-1}"
LR="${LR:-1e-4}"
RES="${RES:-512}"
BLOCKS_SWAP="${BLOCKS_SWAP:-20}"
OPT="${OPT:-AdamW8bit}"
cd /app/sd-scripts

echo ">> 1/2 building _train_anima dataset (reuse pruned WD14 tags, swap trigger -> '$TRIGGER')"
TRAINDIR=/dataset/_train_anima
rm -rf "$TRAINDIR"; mkdir -p "$TRAINDIR/${REPEATS}_${NAME}"
python - "$DATA" "$TRAINDIR/${REPEATS}_${NAME}" "$TRIGGER" <<'PY'
import glob, os, sys, shutil
src, dst, trig = sys.argv[1], sys.argv[2], sys.argv[3].strip()
# The same FIXED-identity prune set as the Illustrious run (safety net — these are already
# pruned in /dataset) PLUS the old Illustrious trigger tokens, so the new trigger owns the look.
PRUNE = {
  "gemma-san","chibi",
  "wings","butterfly wings","fairy wings","insect wings","purple wings","pink wings","demon wings",
  "horns","demon horns","curled horns","pointy ears","long pointy ears","pointed ears","elf",
  "tail","demon tail",
  "bodysuit","black bodysuit","latex","latex bodysuit","bodystocking","skin tight","skintight",
  "covered navel","navel","leotard","black leotard","pantyhose","black pantyhose",
  "gloves","black gloves","elbow gloves","thighhighs","black thighhighs","thigh boots","boots","ass",
  "cleavage","breasts","small breasts","medium breasts","large breasts",
  "purple hair","very long hair","long hair","hair between eyes","bangs","blunt bangs","sidelocks","hair intakes",
  "purple eyes","pink eyes","glowing eyes",
  "earrings","single earring","jewelry","piercing","ear piercing",
  "demon girl","succubus","colored skin","dark skin","virtual youtuber",
}
n=0
for txt in sorted(glob.glob(f"{src}/*.txt")):
    base=os.path.splitext(os.path.basename(txt))[0]
    # find the image
    img=None
    for ext in (".png",".jpg",".jpeg",".webp"):
        if os.path.exists(f"{src}/{base}{ext}"): img=f"{src}/{base}{ext}"; break
    if not img: continue
    tags=[x.strip() for x in open(txt).read().split(",") if x.strip()]
    kept=[x for x in tags if x.lower() not in PRUNE]
    open(f"{dst}/{base}.txt","w").write(trig + (", " + ", ".join(kept) if kept else ""))
    shutil.copy(img, f"{dst}/{os.path.basename(img)}")
    n+=1
print(f"   built {n} image+caption pairs in {dst}")
# show a couple so the log proves the trigger swap
for t in sorted(glob.glob(f"{dst}/*.txt"))[:3]:
    print("   e.g.", os.path.basename(t), "->", open(t).read())
PY

# DURATION: in VALIDATION mode (MAX_STEPS set) pass ONLY --max_train_steps; do NOT also pass
# --max_train_epochs (kohya recomputes total steps from epochs and the step cap is ignored).
if [ -n "${MAX_STEPS:-}" ]; then
  DURATION_ARG="--max_train_steps ${MAX_STEPS}"
  echo ">> 2/2 VALIDATION run: max_train_steps=${MAX_STEPS} (health check — no NaN / no OOM / VRAM fits)"
else
  DURATION_ARG="--max_train_epochs ${EPOCHS} --save_every_n_epochs ${SAVE_EVERY}"
  echo ">> 2/2 FULL run: dim=$DIM alpha=$ALPHA lr=$LR res=$RES repeats=$REPEATS epochs=$EPOCHS swap=$BLOCKS_SWAP"
fi

# Anima = DiT + Rectified Flow. TE (Qwen3-0.6B) is ALWAYS frozen -> cache its outputs (big VRAM
# win on 10GB) which means NO --shuffle_caption (cache is per-image, computed once) -> trigger is
# baked as the fixed first token instead. blocks_to_swap + qwen_image_vae_2d + grad-checkpoint +
# cache_latents fit the 2B DiT in the 3080's 10GB. timestep_sampling=sigmoid per the anima guide.
accelerate launch --num_cpu_threads_per_process 1 anima_train_network.py \
  --pretrained_model_name_or_path "$DIT" --qwen3 "$QWEN3" --vae "$VAE" \
  --train_data_dir "$TRAINDIR" \
  --output_dir "$OUT" --output_name "$NAME" --save_model_as safetensors \
  --resolution "${RES},${RES}" --enable_bucket --min_bucket_reso 256 --max_bucket_reso 768 \
  --network_module networks.lora_anima --network_dim "$DIM" --network_alpha "$ALPHA" \
  --network_train_unet_only \
  --train_batch_size 1 --gradient_checkpointing \
  --mixed_precision bf16 --save_precision bf16 \
  --cache_latents --cache_latents_to_disk \
  --cache_text_encoder_outputs --cache_text_encoder_outputs_to_disk \
  --qwen_image_vae_2d --blocks_to_swap "$BLOCKS_SWAP" --attn_mode torch \
  --optimizer_type "$OPT" --learning_rate "$LR" --max_grad_norm 1.0 \
  --lr_scheduler constant --timestep_sampling sigmoid \
  --caption_extension .txt --keep_tokens 1 \
  --max_data_loader_n_workers 2 --seed 42 \
  $DURATION_ARG

echo ">> done -> $OUT/${NAME}.safetensors (+ epoch checkpoints ${NAME}-NNNNNN.safetensors)"
