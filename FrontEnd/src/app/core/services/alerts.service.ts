import { Injectable, computed, signal } from '@angular/core';
import { Alerta, Severidade } from '../models/alerta.model';

@Injectable({ providedIn: 'root' })
export class AlertsService {
  private readonly _alertas = signal<Alerta[]>([]);
  readonly alertas = this._alertas.asReadonly();
  readonly alertaCritico = computed(() =>
    this._alertas().find((a) => a.severidade === 'critical' && a.requerConfirmacao) ?? null,
  );

  emitir(severidade: Severidade, titulo: string, mensagem: string, requerConfirmacao = false): void {
    const novo: Alerta = {
      id: crypto.randomUUID(),
      severidade,
      titulo,
      mensagem,
      timestamp: Date.now(),
      requerConfirmacao,
    };
    this._alertas.update((lista) => [novo, ...lista].slice(0, 50));
  }

  confirmar(id: string): void {
    this._alertas.update((lista) => lista.filter((a) => a.id !== id));
  }

  limpar(): void {
    this._alertas.set([]);
  }
}
