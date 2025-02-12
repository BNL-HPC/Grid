/*************************************************************************************

    Grid physics library, www.github.com/paboyle/Grid 

    Source file: ./tests/Test_dwf_hdcr.cc

    Copyright (C) 2015

Author: Antonin Portelli <antonin.portelli@me.com>
Author: Peter Boyle <paboyle@ph.ed.ac.uk>
Author: paboyle <paboyle@ph.ed.ac.uk>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

    See the full license in the file "LICENSE" in the top level distribution directory
    *************************************************************************************/
    /*  END LEGAL */
#include <Grid/Grid.h>
#include <Grid/algorithms/iterative/PrecGeneralisedConjugateResidual.h>
#include <Grid/algorithms/iterative/PrecGeneralisedConjugateResidualNonHermitian.h>
#include <Grid/algorithms/iterative/BiCGSTAB.h>

using namespace std;
using namespace Grid;
/* Params
 * Grid: 
 * block1(4)
 * block2(4)
 * 
 * Subspace
 * * Fine  : Subspace(nbasis,hi,lo,order,first,step) -- 32, 60,0.02,500,100,100
 * * Coarse: Subspace(nbasis,hi,lo,order,first,step) -- 32, 18,0.02,500,100,100

 * Smoother:
 * * Fine: Cheby(hi, lo, order)            --  60,0.5,10
 * * Coarse: Cheby(hi, lo, order)          --  12,0.1,4

 * Lanczos:
 * CoarseCoarse IRL( Nk, Nm, Nstop, poly(lo,hi,order))   24,36,24,0.002,4.0,61 
 */

template<class Field> class SolverWrapper : public LinearFunction<Field> {
private:
  LinearOperatorBase<Field> & _Matrix;
  OperatorFunction<Field> & _Solver;
  LinearFunction<Field>   & _Guess;
public:
  using LinearFunction<Field>::operator();

  /////////////////////////////////////////////////////
  // Wrap the usual normal equations trick
  /////////////////////////////////////////////////////
  SolverWrapper(LinearOperatorBase<Field> &Matrix,
	      OperatorFunction<Field> &Solver,
	      LinearFunction<Field> &Guess) 
   :  _Matrix(Matrix), _Solver(Solver), _Guess(Guess) {}; 

  void operator() (const Field &in, Field &out){
 
    _Guess(in,out);
    _Solver(_Matrix,in,out);  // Mdag M out = Mdag in

  }     
};


// Must use a non-hermitian solver
template<class Matrix,class Field>
class PVdagMLinearOperator : public LinearOperatorBase<Field> {
  Matrix &_Mat;
  Matrix &_PV;
public:
  PVdagMLinearOperator(Matrix &Mat,Matrix &PV): _Mat(Mat),_PV(PV){};

  void OpDiag (const Field &in, Field &out) {
    assert(0);
  }
  void OpDir  (const Field &in, Field &out,int dir,int disp) {
    assert(0);
  }
  void OpDirAll  (const Field &in, std::vector<Field> &out){
    assert(0);
  };
  void Op     (const Field &in, Field &out){
    Field tmp(in.Grid());
    _Mat.M(in,tmp);
    _PV.Mdag(tmp,out);
  }
  void AdjOp     (const Field &in, Field &out){
    Field tmp(in.Grid());
    _PV.M(tmp,out);
    _Mat.Mdag(in,tmp);
  }
  void HermOpAndNorm(const Field &in, Field &out,RealD &n1,RealD &n2){
    assert(0);
  }
  void HermOp(const Field &in, Field &out){
    assert(0);
  }
};


RealD InverseApproximation(RealD x){
  return 1.0/x;
}

template<class Field,class Matrix> class ChebyshevSmoother : public LinearFunction<Field>
{
public:
  using LinearFunction<Field>::operator();
  typedef LinearOperatorBase<Field>                            FineOperator;
  Matrix         & _SmootherMatrix;
  FineOperator   & _SmootherOperator;
  
  Chebyshev<Field> Cheby;

  ChebyshevSmoother(RealD _lo,RealD _hi,int _ord, FineOperator &SmootherOperator,Matrix &SmootherMatrix) :
    _SmootherOperator(SmootherOperator),
    _SmootherMatrix(SmootherMatrix),
    Cheby(_lo,_hi,_ord,InverseApproximation)
  {};

  void operator() (const Field &in, Field &out) 
  {
    Field tmp(in.Grid());
    MdagMLinearOperator<Matrix,Field>   MdagMOp(_SmootherMatrix); 
    _SmootherOperator.AdjOp(in,tmp);
    Cheby(MdagMOp,tmp,out);         
  }
};

template<class Field,class Matrix> class MirsSmoother : public LinearFunction<Field>
{
public:
  typedef LinearOperatorBase<Field>                            FineOperator;
  Matrix         & SmootherMatrix;
  FineOperator   & SmootherOperator;
  RealD tol;
  RealD shift;
  int   maxit;

  MirsSmoother(RealD _shift,RealD _tol,int _maxit,FineOperator &_SmootherOperator,Matrix &_SmootherMatrix) :
    shift(_shift),tol(_tol),maxit(_maxit),
    SmootherOperator(_SmootherOperator),
    SmootherMatrix(_SmootherMatrix)
  {};

  void operator() (const Field &in, Field &out) 
  {
    ZeroGuesser<Field> Guess;
    ConjugateGradient<Field>  CG(tol,maxit,false);
 
    Field src(in.Grid());

    ShiftedMdagMLinearOperator<SparseMatrixBase<Field>,Field> MdagMOp(SmootherMatrix,shift);
    SmootherOperator.AdjOp(in,src);
    Guess(src,out);
    CG(MdagMOp,src,out); 
  }
};

#define GridLogLevel std::cout << GridLogMessage <<std::string(level,'\t')<< " Level "<<level <<" "

template<class Fobj,class CComplex,int nbasis, class CoarseSolver>
class HDCRPreconditioner : public LinearFunction< Lattice<Fobj> > {
public:
  using LinearFunction<Lattice<Fobj> >::operator();

  typedef Aggregation<Fobj,CComplex,nbasis> Aggregates;
  typedef CoarsenedMatrix<Fobj,CComplex,nbasis> CoarseOperator;
  typedef typename Aggregation<Fobj,CComplex,nbasis>::CoarseVector CoarseVector;
  typedef typename Aggregation<Fobj,CComplex,nbasis>::CoarseMatrix CoarseMatrix;
  typedef typename Aggregation<Fobj,CComplex,nbasis>::FineField    FineField;
  typedef LinearOperatorBase<FineField>                            FineOperator;
  typedef LinearFunction    <FineField>                            FineSmoother;

  Aggregates     & _Aggregates;
  FineOperator   & _FineOperator;
  FineSmoother   & _Smoother;
  CoarseSolver   & _CoarseSolve;

  int    level;  void Level(int lv) {level = lv; };


  HDCRPreconditioner(Aggregates &Agg,
		     FineOperator &Fine,
		     FineSmoother &Smoother,
		     CoarseSolver &CoarseSolve_)
    : _Aggregates(Agg),
      _FineOperator(Fine),
      _Smoother(Smoother),
      _CoarseSolve(CoarseSolve_),
      level(1)  {  }

  virtual void operator()(const FineField &in, FineField & out) 
  {
    auto CoarseGrid = _Aggregates.CoarseGrid;
    CoarseVector Csrc(CoarseGrid);
    CoarseVector Csol(CoarseGrid);
    FineField vec1(in.Grid());
    FineField vec2(in.Grid());

    double t;
    // Fine Smoother
    t=-usecond();
    _Smoother(in,out);
    t+=usecond();
    GridLogLevel << "Smoother took "<< t/1000.0<< "ms" <<std::endl;

    // Update the residual
    _FineOperator.Op(out,vec1);  sub(vec1, in ,vec1);   

    // Fine to Coarse 
    t=-usecond();
    _Aggregates.ProjectToSubspace  (Csrc,vec1);
    t+=usecond();
    GridLogLevel << "Project to coarse took "<< t/1000.0<< "ms" <<std::endl;

    // Coarse correction
    t=-usecond();
    _CoarseSolve(Csrc,Csol);
    t+=usecond();
    GridLogLevel << "Coarse solve took "<< t/1000.0<< "ms" <<std::endl;

    // Coarse to Fine
    t=-usecond();
    _Aggregates.PromoteFromSubspace(Csol,vec1); 
    add(out,out,vec1);
    t+=usecond();
    GridLogLevel << "Promote to this level took "<< t/1000.0<< "ms" <<std::endl;

    // Residual
    _FineOperator.Op(out,vec1);  sub(vec1 ,in , vec1);  

    // Fine Smoother
    t=-usecond();
    _Smoother(vec1,vec2);
    t+=usecond();
    GridLogLevel << "Smoother took "<< t/1000.0<< "ms" <<std::endl;

    add( out,out,vec2);
  }
};

/*
template<class Fobj,class CComplex,int nbasis, class Guesser, class CoarseSolver>
class MultiGridPreconditioner : public LinearFunction< Lattice<Fobj> > {
public:

  typedef Aggregation<Fobj,CComplex,nbasis> Aggregates;
  typedef CoarsenedMatrix<Fobj,CComplex,nbasis> CoarseOperator;
  typedef typename Aggregation<Fobj,CComplex,nbasis>::CoarseVector CoarseVector;
  typedef typename Aggregation<Fobj,CComplex,nbasis>::CoarseMatrix CoarseMatrix;
  typedef typename Aggregation<Fobj,CComplex,nbasis>::FineField    FineField;
  typedef LinearOperatorBase<FineField>                            FineOperator;
  typedef LinearFunction    <FineField>                            FineSmoother;

  Aggregates     & _Aggregates;
  CoarseOperator & _CoarseOperator;
  FineOperator   & _FineOperator;
  Guesser        & _Guess;
  FineSmoother   & _Smoother;
  CoarseSolver   & _CoarseSolve;

  int    level;  void Level(int lv) {level = lv; };


  MultiGridPreconditioner(Aggregates &Agg, CoarseOperator &Coarse, 
			  FineOperator &Fine,
			  FineSmoother &Smoother,
			  Guesser &Guess_,
			  CoarseSolver &CoarseSolve_)
    : _Aggregates(Agg),
      _CoarseOperator(Coarse),
      _FineOperator(Fine),
      _Smoother(Smoother),
      _Guess(Guess_),
      _CoarseSolve(CoarseSolve_),
      level(1)  {  }

  virtual void operator()(const FineField &in, FineField & out) 
  {
    CoarseVector Csrc(_CoarseOperator.Grid());
    CoarseVector Csol(_CoarseOperator.Grid()); 
    FineField vec1(in.Grid());
    FineField vec2(in.Grid());

    double t;
    // Fine Smoother
    t=-usecond();
    _Smoother(in,out);
    t+=usecond();
    GridLogLevel << "Smoother took "<< t/1000.0<< "ms" <<std::endl;

    // Update the residual
    _FineOperator.Op(out,vec1);  sub(vec1, in ,vec1);   

    // Fine to Coarse 
    t=-usecond();
    _Aggregates.ProjectToSubspace  (Csrc,vec1);
    t+=usecond();
    GridLogLevel << "Project to coarse took "<< t/1000.0<< "ms" <<std::endl;

    // Coarse correction
    t=-usecond();
    _CoarseSolve(Csrc,Csol);
    t+=usecond();
    GridLogLevel << "Coarse solve took "<< t/1000.0<< "ms" <<std::endl;

    // Coarse to Fine
    t=-usecond();
    _Aggregates.PromoteFromSubspace(Csol,vec1); 
    add(out,out,vec1);
    t+=usecond();
    GridLogLevel << "Promote to this level took "<< t/1000.0<< "ms" <<std::endl;

    // Residual
    _FineOperator.Op(out,vec1);  sub(vec1 ,in , vec1);  

    // Fine Smoother
    t=-usecond();
    _Smoother(vec1,vec2);
    t+=usecond();
    GridLogLevel << "Smoother took "<< t/1000.0<< "ms" <<std::endl;

    add( out,out,vec2);
  }
};
*/

int main (int argc, char ** argv)
{
  Grid_init(&argc,&argv);

  const int Ls=16;

  GridCartesian         * UGrid   = SpaceTimeGrid::makeFourDimGrid(GridDefaultLatt(), GridDefaultSimd(Nd,vComplex::Nsimd()),GridDefaultMpi());
  GridRedBlackCartesian * UrbGrid = SpaceTimeGrid::makeFourDimRedBlackGrid(UGrid);

  GridCartesian         * FGrid   = SpaceTimeGrid::makeFiveDimGrid(Ls,UGrid);
  GridRedBlackCartesian * FrbGrid = SpaceTimeGrid::makeFiveDimRedBlackGrid(Ls,UGrid);

  ///////////////////////////////////////////////////
  // Construct a coarsened grid; utility for this?
  ///////////////////////////////////////////////////
  std::vector<int> block ({2,2,2,2});
  std::vector<int> blockc ({2,2,2,2});
  const int nbasis= 32;
  const int nbasisc= 32;
  auto clatt = GridDefaultLatt();
  for(int d=0;d<clatt.size();d++){
    clatt[d] = clatt[d]/block[d];
  }
  auto cclatt = clatt;
  for(int d=0;d<clatt.size();d++){
    cclatt[d] = clatt[d]/blockc[d];
  }

  GridCartesian *Coarse4d =  SpaceTimeGrid::makeFourDimGrid(clatt, GridDefaultSimd(Nd,vComplex::Nsimd()),GridDefaultMpi());;
  GridCartesian *Coarse5d =  SpaceTimeGrid::makeFiveDimGrid(1,Coarse4d);
  GridCartesian *CoarseCoarse4d =  SpaceTimeGrid::makeFourDimGrid(cclatt, GridDefaultSimd(Nd,vComplex::Nsimd()),GridDefaultMpi());;
  GridCartesian *CoarseCoarse5d =  SpaceTimeGrid::makeFiveDimGrid(1,CoarseCoarse4d);

  GridRedBlackCartesian * Coarse4dRB = SpaceTimeGrid::makeFourDimRedBlackGrid(Coarse4d);
  GridRedBlackCartesian * Coarse5dRB = SpaceTimeGrid::makeFiveDimRedBlackGrid(1,Coarse4d);
  GridRedBlackCartesian *CoarseCoarse4dRB = SpaceTimeGrid::makeFourDimRedBlackGrid(CoarseCoarse4d);
  GridRedBlackCartesian *CoarseCoarse5dRB = SpaceTimeGrid::makeFiveDimRedBlackGrid(1,CoarseCoarse4d);

  std::vector<int> seeds4({1,2,3,4});
  std::vector<int> seeds5({5,6,7,8});
  std::vector<int> cseeds({5,6,7,8});
  GridParallelRNG          RNG5(FGrid);   RNG5.SeedFixedIntegers(seeds5);
  GridParallelRNG          RNG4(UGrid);   RNG4.SeedFixedIntegers(seeds4);
  GridParallelRNG          CRNG(Coarse5d);CRNG.SeedFixedIntegers(cseeds);
  LatticeFermion    src(FGrid); gaussian(RNG5,src);// src=src+g5*src;
  LatticeFermion result(FGrid); 
  LatticeGaugeField Umu(UGrid); 

  FieldMetaData header;
  std::string file("./ckpoint_lat.4000");
  NerscIO::readConfiguration(Umu,header,file);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << "Building g5R5 hermitian DWF operator" <<std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  RealD mass=0.001;
  RealD M5=1.8;
  DomainWallFermionR Ddwf(Umu,*FGrid,*FrbGrid,*UGrid,*UrbGrid,mass,M5);
  DomainWallFermionR Dpv (Umu,*FGrid,*FrbGrid,*UGrid,*UrbGrid,1.0,M5);

  typedef Aggregation<vSpinColourVector,vTComplex,nbasis>              Subspace;
  typedef CoarsenedMatrix<vSpinColourVector,vTComplex,nbasis>          CoarseOperator;
  typedef CoarseOperator::CoarseVector                                 CoarseVector;
  typedef CoarseOperator::siteVector siteVector;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << "Calling Aggregation class to build subspace" <<std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  MdagMLinearOperator<DomainWallFermionR,LatticeFermion> HermDefOp(Ddwf);

  Subspace Aggregates(Coarse5d,FGrid,0);

  assert ( (nbasis & 0x1)==0);
  {
    int nb=nbasis/2;
    Aggregates.CreateSubspaceChebyshev(RNG5,HermDefOp,nb,60.0,0.02,500,100,100,0.0);
    for(int n=0;n<nb;n++){
      G5R5(Aggregates.subspace[n+nb],Aggregates.subspace[n]);
    }
    LatticeFermion A(FGrid);
    LatticeFermion B(FGrid);
    for(int n=0;n<nb;n++){
      A = Aggregates.subspace[n];
      B = Aggregates.subspace[n+nb];
      Aggregates.subspace[n]   = A+B; // 1+G5 // eigen value of G5R5 is +1
      Aggregates.subspace[n+nb]= A-B; // 1-G5 // eigen value of G5R5 is -1
    }
  }

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << " Will coarsen G5R5 M and G5R5 Mpv in G5R5 compatible way " <<std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  
  typedef CoarsenedMatrix<vSpinColourVector,vTComplex,nbasis>    Level1Op;
  typedef CoarsenedMatrix<siteVector,iScalar<vTComplex>,nbasisc> Level2Op;

  Gamma5R5HermitianLinearOperator<DomainWallFermionR,LatticeFermion> HermIndefOp(Ddwf);
  Gamma5R5HermitianLinearOperator<DomainWallFermionR,LatticeFermion> HermIndefOpPV(Dpv);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << "Building coarse representation of Indef operator" <<std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;

  Level1Op LDOp(*Coarse5d,*Coarse5dRB,1);   LDOp.CoarsenOperator(FGrid,HermIndefOp,Aggregates);
  Level1Op LDOpPV(*Coarse5d,*Coarse5dRB,1); LDOpPV.CoarsenOperator(FGrid,HermIndefOpPV,Aggregates);


  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << " Testing fine and coarse solvers " <<std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;

  CoarseVector c_src(Coarse5d); c_src=1.0;
  CoarseVector c_res(Coarse5d);

  LatticeFermion f_src(FGrid); f_src=1.0;
  LatticeFermion f_res(FGrid);

  LatticeFermion f_src_e(FrbGrid); f_src_e=1.0;
  LatticeFermion f_res_e(FrbGrid);

  RealD tol=1.0e-8;
  int MaxIt = 10000;

  BiCGSTAB<CoarseVector>                     CoarseBiCGSTAB(tol,MaxIt);
  ConjugateGradient<CoarseVector>            CoarseCG(tol,MaxIt);
  //  GeneralisedMinimalResidual<CoarseVector>   CoarseGMRES(tol,MaxIt,20);

  BiCGSTAB<LatticeFermion>                   FineBiCGSTAB(tol,MaxIt);
  ConjugateGradient<LatticeFermion>          FineCG(tol,MaxIt);
  //  GeneralisedMinimalResidual<LatticeFermion> FineGMRES(tol,MaxIt,20);
  
  MdagMLinearOperator<DomainWallFermionR,LatticeFermion>    FineMdagM(Ddwf);     //  M^\dag M
  PVdagMLinearOperator<DomainWallFermionR,LatticeFermion>   FinePVdagM(Ddwf,Dpv);//  M_{pv}^\dag M
  SchurDiagMooeeOperator<DomainWallFermionR,LatticeFermion> FineDiagMooee(Ddwf); //  M_ee - Meo Moo^-1 Moe 
  SchurDiagOneOperator<DomainWallFermionR,LatticeFermion>   FineDiagOne(Ddwf);   //  1 - M_ee^{-1} Meo Moo^{-1} Moe e

   MdagMLinearOperator<Level1Op,CoarseVector> CoarseMdagM(LDOp);
  PVdagMLinearOperator<Level1Op,CoarseVector> CoarsePVdagM(LDOp,LDOpPV);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << "Fine CG unprec "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;

  f_res=Zero();
  FineCG(FineMdagM,f_src,f_res);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << "Fine CG prec DiagMooee "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  
  f_res_e=Zero();
  FineCG(FineDiagMooee,f_src_e,f_res_e);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << "Fine CG prec DiagOne "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;

  f_res_e=Zero();
  FineCG(FineDiagOne,f_src_e,f_res_e);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << "Fine BiCGSTAB unprec "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;

  f_res=Zero();
  FineBiCGSTAB(FinePVdagM,f_src,f_res);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << "Coarse BiCGSTAB "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  
  c_res=Zero();
  CoarseBiCGSTAB(CoarsePVdagM,c_src,c_res);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << "Coarse CG unprec "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;

  c_res=Zero();
  CoarseCG(CoarseMdagM,c_src,c_res);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << " Running Coarse grid Lanczos "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;

  Chebyshev<CoarseVector>      IRLCheby(0.03,12.0,71);  // 1 iter
  FunctionHermOp<CoarseVector> IRLOpCheby(IRLCheby,CoarseMdagM);
  PlainHermOp<CoarseVector>    IRLOp    (CoarseMdagM);
  int Nk=64;
  int Nm=128;
  int Nstop=Nk;
  ImplicitlyRestartedLanczos<CoarseVector> IRL(IRLOpCheby,IRLOp,Nstop,Nk,Nm,1.0e-3,20);

  int Nconv;
  std::vector<RealD>            eval(Nm);
  std::vector<CoarseVector>     evec(Nm,Coarse5d);
  IRL.calc(eval,evec,c_src,Nconv);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << " Running Coarse grid deflated solver              "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;

  DeflatedGuesser<CoarseVector> DeflCoarseGuesser(evec,eval);
  NormalEquations<CoarseVector> DeflCoarseCGNE   (LDOp,CoarseCG,DeflCoarseGuesser);
  c_res=Zero();
  DeflCoarseCGNE(c_src,c_res);


  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << " Running HDCR                                     "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;

  ConjugateGradient<CoarseVector>  CoarseMgridCG(0.001,1000);     
  ChebyshevSmoother<LatticeFermion,DomainWallFermionR> FineSmoother(0.5,60.0,10,HermIndefOp,Ddwf);

  typedef HDCRPreconditioner<vSpinColourVector,  vTComplex,nbasis, NormalEquations<CoarseVector> >   TwoLevelHDCR;
  TwoLevelHDCR TwoLevelPrecon(Aggregates,
			      HermIndefOp,
			      FineSmoother,
			      DeflCoarseCGNE);
  TwoLevelPrecon.Level(1);
  //  PrecGeneralisedConjugateResidual<LatticeFermion> l1PGCR(1.0e-8,100,HermIndefOp,TwoLevelPrecon,16,16);
  PrecGeneralisedConjugateResidualNonHermitian<LatticeFermion> l1PGCR(1.0e-8,100,HermIndefOp,TwoLevelPrecon,16,16);
  l1PGCR.Level(1);

  f_res=Zero();

  CoarseCG.Tolerance=0.02;
  l1PGCR(f_src,f_res);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << " Running Multigrid                                "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;

  BiCGSTAB<CoarseVector>    CoarseMgridBiCGSTAB(0.01,1000);     
  BiCGSTAB<LatticeFermion>  FineMgridBiCGSTAB(0.0,24);
  ZeroGuesser<CoarseVector> CoarseZeroGuesser;
  ZeroGuesser<LatticeFermion> FineZeroGuesser;

  SolverWrapper<LatticeFermion> FineBiCGSmoother(  FinePVdagM,  FineMgridBiCGSTAB,  FineZeroGuesser);
  SolverWrapper<CoarseVector> CoarsePVdagMSolver(CoarsePVdagM,CoarseMgridBiCGSTAB,CoarseZeroGuesser);
  typedef HDCRPreconditioner<vSpinColourVector, vTComplex,nbasis, SolverWrapper<CoarseVector> >  TwoLevelMG;

  TwoLevelMG _TwoLevelMG(Aggregates,
			 FinePVdagM,
			 FineBiCGSmoother,
			 CoarsePVdagMSolver);
  _TwoLevelMG.Level(1);

  PrecGeneralisedConjugateResidualNonHermitian<LatticeFermion> pvPGCR(1.0e-8,100,FinePVdagM,_TwoLevelMG,16,16);
  pvPGCR.Level(1);

  f_res=Zero();
  pvPGCR(f_src,f_res);

  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  std::cout<<GridLogMessage << "Done "<< std::endl;
  std::cout<<GridLogMessage << "**************************************************"<< std::endl;
  Grid_finalize();
  
}
