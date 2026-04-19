import { Injectable, signal } from '@angular/core';

export type Tema = 'dark' | 'light';

const STORAGE_KEY = 'hdrop.theme';

@Injectable({ providedIn: 'root' })
export class ThemeService {
  readonly tema = signal<Tema>(this.carregarInicial());

  constructor() {
    this.aplicar(this.tema());
  }

  alternar(): void {
    this.definir(this.tema() === 'dark' ? 'light' : 'dark');
  }

  definir(novo: Tema): void {
    this.tema.set(novo);
    this.aplicar(novo);
    try {
      localStorage.setItem(STORAGE_KEY, novo);
    } catch {
      // storage pode estar bloqueado (modo privado), sem crise
    }
  }

  private aplicar(t: Tema): void {
    document.body.setAttribute('data-theme', t);
  }

  private carregarInicial(): Tema {
    try {
      const salvo = localStorage.getItem(STORAGE_KEY);
      if (salvo === 'light' || salvo === 'dark') return salvo;
    } catch {}
    return 'dark';
  }
}
