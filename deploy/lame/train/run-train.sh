#!/usr/bin/env bash
# Caption + train a Gemma chibi LoRA — runs INSIDE a trainer container (slop-train / slop-train-cuda):
#   docker exec -e REPEATS=5 -e EPOCHS=8 -i slop-train bash -s < run-train.sh
# Mounts (from deploy): /dataset (images), /models (Illustrious ckpt, ro), /output (loras).
# Config via env (defaults = the ROCm/7800XT path; CUDA/3080 path overrides them):
#   PREC=no|bf16  RES=768|1024  DIM=32|64  ALPHA  OPT=AdamW|AdamW8bit  ATTN=sdpa|xformers  TE_LR
#   ROCm/7800XT (NaN-safe): PREC=no RES=768 (defaults)
#   CUDA/3080 (v2):  PREC=bf16 RES=1024 DIM=64 ALPHA=32 OPT=AdamW8bit ATTN=xformers TE_LR=5e-5
set -euo pipefail

DATA=/dataset
MODEL="${MODEL:-/models/Illustrious-XL-v2.0.safetensors}"
OUT=/output
NAME="${LORA_NAME:-gemma-san-chibi}"
TRIGGER="${TRIGGER:-gemma-san, chibi}"
REPEATS="${REPEATS:-10}"
EPOCHS="${EPOCHS:-8}"
DIM="${DIM:-32}"; ALPHA="${ALPHA:-16}"
OPT="${OPT:-AdamW}"; ATTN="${ATTN:-sdpa}"
# Train the text encoder only when TE_LR is set; otherwise U-Net only (NaN-safer on ROCm).
TE_ARGS="--network_train_unet_only"; [ -n "${TE_LR:-}" ] && TE_ARGS="--text_encoder_lr ${TE_LR}"
cd /app/sd-scripts

echo ">> 1/3 captioning with WD14 (booru tags)"
python finetune/tag_images_by_wd14_tagger.py "$DATA" \
  --repo_id SmilingWolf/wd-v1-4-moat-tagger-v2 --onnx --batch_size 4 \
  --caption_extension .txt --remove_underscore --thresh 0.35

echo ">> 2/3 pruning design tags (trigger owns the design) + prefixing trigger ($TRIGGER)"
python - "$DATA" "$TRIGGER" <<'PY'
import glob, sys
data, trig = sys.argv[1], sys.argv[2].strip()
# Prune the FIXED-identity tags so the trigger word absorbs the design (more reliable than
# spelling out wings/horns/tail in every prompt). Keep pose/view/expression/composition tags
# (1girl, solo, from behind, pointing, smile, simple background, …) so those stay controllable.
PRUNE = {
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
trigset = {p.strip().lower() for p in trig.split(",")}
for t in glob.glob(f"{data}/*.txt"):
    tags = [x.strip() for x in open(t).read().split(",") if x.strip()]
    kept = [x for x in tags if x.lower() not in PRUNE and x.lower() not in trigset]
    open(t, "w").write(trig + (", " + ", ".join(kept) if kept else ""))
print("captions pruned + trigger-prefixed")
PY
TRAINDIR=/dataset/_train
rm -rf "$TRAINDIR"; mkdir -p "$TRAINDIR/${REPEATS}_${NAME}"
cp "$DATA"/*.png "$DATA"/*.jpg "$DATA"/*.jpeg "$DATA"/*.webp "$DATA"/*.txt \
   "$TRAINDIR/${REPEATS}_${NAME}/" 2>/dev/null || true

echo ">> 3/3 training SDXL LoRA (PREC=${PREC:-no} RES=${RES:-768} DIM=$DIM OPT=$OPT ATTN=$ATTN TE=${TE_LR:-off})"
accelerate launch --num_processes 1 sdxl_train_network.py \
  --pretrained_model_name_or_path "$MODEL" \
  --train_data_dir "$TRAINDIR" \
  --output_dir "$OUT" --output_name "$NAME" \
  --resolution "${RES:-768},${RES:-768}" --enable_bucket --min_bucket_reso 512 --max_bucket_reso 1536 \
  --network_module networks.lora --network_dim "$DIM" --network_alpha "$ALPHA" \
  --train_batch_size 1 --gradient_checkpointing --mixed_precision "${PREC:-no}" --save_precision fp16 \
  --cache_latents --cache_latents_to_disk \
  --optimizer_type "$OPT" --unet_lr 1e-4 $TE_ARGS --max_grad_norm 1.0 \
  --lr_scheduler cosine --max_train_epochs "$EPOCHS" \
  --caption_extension .txt --shuffle_caption --keep_tokens 2 \
  --save_every_n_epochs "${SAVE_EVERY:-$EPOCHS}" \
  --"$ATTN" --no_half_vae --seed 42

echo ">> done -> $OUT/${NAME}.safetensors"
