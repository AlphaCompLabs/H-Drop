import { HttpClient } from '@angular/common/http';
import { Injectable, inject } from '@angular/core';
import { Observable, catchError, of } from 'rxjs';
import { environment } from '../../../environments/environment';
import { ComandoDestino, ComandoResposta } from '../models/comando.model';

@Injectable({ providedIn: 'root' })
export class ComandoService {
  private readonly http = inject(HttpClient);
  private readonly base = environment.apiBaseUrl;

  enviarDestino(destino: ComandoDestino): Observable<ComandoResposta | null> {
    return this.http
      .post<ComandoResposta>(`${this.base}/comando`, destino)
      .pipe(catchError(() => of(null)));
  }

  ultimoDestino(): Observable<ComandoDestino | null> {
    return this.http
      .get<ComandoDestino | null>(`${this.base}/comando/ultimo`)
      .pipe(catchError(() => of(null)));
  }

  statusSistema(): Observable<unknown> {
    return this.http.get(`${this.base}/status`).pipe(catchError(() => of(null)));
  }
}
