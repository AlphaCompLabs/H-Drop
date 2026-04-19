import { Routes } from '@angular/router';

export const routes: Routes = [
  {
    path: '',
    loadComponent: () =>
      import('./features/mission-control/mission-control.component').then(
        (m) => m.MissionControlComponent,
      ),
  },
  { path: '**', redirectTo: '' },
];
