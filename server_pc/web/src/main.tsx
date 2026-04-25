import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import "@shoelace-style/shoelace/dist/themes/light.css";
import { setBasePath } from "@shoelace-style/shoelace/dist/utilities/base-path.js";
import { App } from "./App";
import "./index.css";

setBasePath("https://cdn.jsdelivr.net/npm/@shoelace-style/shoelace@2.19.1/cdn/");

const root = document.getElementById("root");
if (!root) {
  throw new Error("root element missing");
}

createRoot(root).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
