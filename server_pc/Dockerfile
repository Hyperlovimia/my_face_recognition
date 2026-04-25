# 阶段 1：在 Linux 下用 Node 构建 React（Vite 产物输出到与 web 同级的 static/）
FROM node:20-bookworm-slim AS frontend
WORKDIR /src/web
COPY web/package.json ./
RUN npm install
COPY web/ ./
RUN npm run build

# 阶段 2：仅复制 Python 与构建后的静态资源
FROM python:3.11-slim
WORKDIR /app

COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt

COPY app /app/app
COPY --from=frontend /src/static /app/static

EXPOSE 8000

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
